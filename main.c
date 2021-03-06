/*
 * main.c
 * LulSynth
 *
 * Created by Allek Mott on 12/29/14.
 * Copyright (c) 2014 Allek Mott. All rights reserved.
 */

#include <stdio.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "waves.h"
#include "portaudio.h"

#define SAMPLE_RATE     (44100)
#define VOLUME          (0.5f)
#define FRAMES_PER_BUF  (256)

/* buffer or not */
int buffer = 0;

/* number of current sample (last sample generated) */
int sample_num = 0;

/* fat buffer containing a second's worth of samples
 * used in callback function
 */
float *sample_buffer;

/* number of initial sample in buffer */
int buf_init_sample_num;

/* ghetto input variable */
float a = 440.0f;

/* pointer to current wave function
 * If input designates a change in wave type (triangle, square, etc),
 * wave pointer will be reassigned to corresponding wave function.
 */
float (*wave) (float) = &sine;

/* Data structure to contain stereo sample data */
typedef struct
{
    float left_phase;
    float right_phase;
}
sampleData;

/* Check for portaudio error */
void pa_errcheck(PaError *err) {
    if (*err == paNoError) {
        return;
    } else {
        Pa_Terminate();
        fprintf(stderr,
                "Apparently portaudio decided to suck.\n"\
                "ErrNo: %d\n"\
                "Message: %s\n",
                *err,
                Pa_GetErrorText(*err));
        exit(*err);
    }
}

/* THREAD STUFF */

PaStream *stream;
PaError pa_err;

pthread_t inputThread;
pthread_t bufferThread;

void clean_exit() {
    pthread_join(bufferThread, NULL);
    
    /* close up portaudio */
    pa_err = Pa_StopStream(stream);
    pa_errcheck(&pa_err);
    
    pa_err = Pa_CloseStream(stream);
    pa_errcheck(&pa_err);
    
    Pa_Terminate();
    free(sample_buffer);
    printf("All done.\n");
    
    /* exit input thread (hopefully) */
    pthread_exit(NULL);
}

void sighandler(int signo) {
    if (signo == SIGINT) {
        printf("Interrupted, exiting.\n");
        clean_exit();
    }
}

/* Thread to grab textual input */
void *grab_input(void *arg) {
    printf("I'm the input thread!\n");
    char input[30];
    while (1) {
        fgets(input, 30, stdin);
        
        switch (input[0]) {
            case 'a': /* sine */
                wave = &sine;
                printf("Sine wave!\n");
                continue;
            case 'b': /* triangle */
                wave = &triangle;
                printf("Triangle wave!\n");
                continue;
            case 'c': /* square */
                wave = &square;
                printf("Square wave!\n");
                continue;
        }
        
        float input_f = atof(input);
        if (input_f == NAN)
            input_f = 0;
        a = atof(input);
    }
}

/* Generate input value for wave function using provided time */
float gen_finput(float t) {
    return (((255.0 - a)/255.0f) * sine(140.0f * t) +   // red
            ((255.0 - 2*a)/255.0f) * sine(180.0f * t) +   // green
            ((255.0 - 3*a)/255.0f) * sine(220.0f * t))    // blue
            / 3.0f;
    //return (sin(a * t) + .5f * sin(.5f * a * t + t) + .25f * sin(.25f * a * t)) / 2.0f;
}


/* Generate 1s worth of samples and throw them all into
 * a nice, pretty buffer.
 *
 * @param init_sample_num sample number represented by index 0 in
 *        new buffer
 */
float *gen_buf(int init_sample_num) {
    float *buffer = malloc(sizeof(float) * SAMPLE_RATE);
    
    /* number of gerated samples */
    int gend_samples;
    
    /* generate a second's worth of samples, throw them in the buffer */
    for (gend_samples = 0; gend_samples < SAMPLE_RATE; gend_samples++) {
        /* current time, based upon sample thingy */
        float t = ((float) (init_sample_num + gend_samples) / SAMPLE_RATE);
        float finput = gen_finput(t);
        buffer[gend_samples] = wave(finput);
    }
    
    return buffer;
}


/* Buffering for audio output.
 * NEW MODEL:
 * generate fat buffer (1 second worth)
 * keep track of position of buffer in time
 * when callback has almost exhausted buffer,
 *  - generate new one behind the scenes, with
 *    sufficient overlap to scrap old buffer
 * if input vars change:
 *  - generate new buffer, leave old alone until
 *    new is generated
 */
void *buffer_dat_shiz(void *arg) {
    printf("I'm the buffer thread!\n");
    
    /* Allocate 1s worth of sample memory, starting from t = 0 */
    sample_buffer = gen_buf(buf_init_sample_num = 0);
    
    float last_a = a;
    void *last_wave = wave;
    
    while (1) {
        if (a != last_a || wave != last_wave) { /* input var changed */
            printf("Buffer regen, init_sample_num = %i\n", buf_init_sample_num);
            last_a = a; last_wave = wave;
            float *new_buffer = gen_buf(buf_init_sample_num);
            
            /* wait until callback has copied frame to free old buffer */
            while (sample_num % 256 != 0)
                usleep(23);
            
            free(sample_buffer);
            sample_buffer = new_buffer;
        } else if (sample_num > (buf_init_sample_num + (SAMPLE_RATE / 2))) { /* 1/2 s through buffer */
            int new_init_sample_num = buf_init_sample_num + (SAMPLE_RATE / 2);
            
            printf("Buffer regen\n\tsample_no = %i\n\tinit_sample_num = %i\n", sample_num, new_init_sample_num);
            
            float *new_buffer = gen_buf(buf_init_sample_num = new_init_sample_num);
            
            float *old_buffer_pointer = sample_buffer;
            sample_buffer = new_buffer;
            
            // free after swap, no hole when freed
            free(old_buffer_pointer);
        }
        /* sleep 1/4 */
        usleep(250000);
    }
    
    return NULL;
}

static int synth_callback(const void *inbuf,
                          void *outbuf,
                          unsigned long fpb, /* frames per buffer */
                          const PaStreamCallbackTimeInfo *time_info,
                          PaStreamCallbackFlags callback_flags,
                          void *usample) {
    /* pointer to frame inside of buffer */
    float *frame = sample_buffer + (sample_num - buf_init_sample_num);
    
    sampleData *sample = (sampleData*) usample;
    float *out = (float*) outbuf;
    unsigned int i;
    (void) inbuf;
    
    if (buffer)
        for (i=0; i<fpb; i++) {
            /* assign sample values to output array, increment index */
            *out++ = sample->left_phase;
            *out++ = sample->right_phase;
            
            sample->left_phase = sample->right_phase = frame[i];
            sample_num++;
        
            sample->left_phase *= VOLUME;
            sample->right_phase *= VOLUME;
        }
    else
        for (i=0; i<fpb; i++) {
            /* assign sample values to output array, increment index */
            *out++ = sample->left_phase;
            *out++ = sample->right_phase;
            
            float t = ((float) ++sample_num / SAMPLE_RATE);
            float finput = gen_finput(t);
            sample->left_phase = sample->right_phase = wave(finput);
            
            sample->left_phase *= VOLUME;
            sample->right_phase *= VOLUME;
        }
    
    return 0;
}

static sampleData sample;
int main(int argc, char *argv[]) {
    if (signal(SIGINT, sighandler) == SIG_ERR)
        printf("\ncan't catch SIGINT\n");
    
    
    srand((unsigned int) time(NULL));
    printf("Initializing PortAudio\n");
    
    /* init sample data structure */
    sample.left_phase = sample.right_phase = 0.0f;
    
    /* init PortAudio library */
    pa_err = Pa_Initialize();
    pa_errcheck(&pa_err);
    
    /* open stream */
    pa_err = Pa_OpenDefaultStream(&stream,         /* stream pointer               */
                               0,               /* no input channels            */
                               2,               /* stereo (2 output channels)   */
                               paFloat32,       /* 32bits per sample            */
                               SAMPLE_RATE,     /* sample rate                  */
                               FRAMES_PER_BUF,  /* frames per buffer            */
                               synth_callback,  /* callback function            */
                               &sample);        /* sample data structure        */
    pa_errcheck(&pa_err);
    
    /* begin writing to stream */
    pa_err = Pa_StartStream(stream);
    pa_errcheck(&pa_err);
    
    pthread_create(&inputThread, NULL, grab_input, NULL);
    if (buffer) {
        pthread_create(&bufferThread, NULL, buffer_dat_shiz, NULL);
        pthread_join(bufferThread, NULL);
    }
    clean_exit();
}