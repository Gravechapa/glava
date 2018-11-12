#ifdef GLAVA_JACK_SUPPORT
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <jack/jack.h>

#include "fifo.h"
#include "jack_input.h"

enum client_state {
    WORKING,
    PREPARING_TO_TERMINATE,
    TERMINATING
};

struct jack_input {
    jack_port_t *left_input_port;
    jack_port_t *right_input_port;
    jack_client_t *client;
    struct audio_data *audio;

    enum client_state state;
    pthread_t monitoring_thread;
    pthread_spinlock_t state_sync;
    pthread_barrier_t barrier;

    bool verbose;
};


int process(jack_nframes_t nframes, void *arg) {

    struct jack_input* jack = (struct jack_input*) arg;

    pthread_spin_lock(&jack->state_sync);
    if (jack->state == PREPARING_TO_TERMINATE) {
        jack->state = TERMINATING;
        pthread_barrier_wait(&jack->barrier);
        pthread_spin_unlock(&jack->state_sync);
        return 0;
    }
    pthread_spin_unlock(&jack->state_sync);

    float* bl = (float*) jack->audio->audio_out_l;
    float* br = (float*) jack->audio->audio_out_r;
    size_t fsz = jack->audio->audio_buf_sz;

    pthread_mutex_lock(&jack->audio->mutex);

    memmove(bl, &bl[nframes], (fsz - nframes) * sizeof(float));
    memmove(br, &br[nframes], (fsz - nframes) * sizeof(float));

    jack_default_audio_sample_t *left_in, *right_in;

    left_in = (jack_default_audio_sample_t*) jack_port_get_buffer (jack->left_input_port, nframes);
    if (jack->right_input_port) {
        right_in = (jack_default_audio_sample_t*) jack_port_get_buffer (jack->right_input_port, nframes);
    }

    for (unsigned int i = 0; i < nframes; ++i) {

        size_t idx = (fsz - nframes) + i;
        bl[idx] = left_in[i];

        if (jack->audio->channels == 1) {
            br[idx] = left_in[i];
        }

        /* stereo storing channels in buffer */
        if (jack->audio->channels == 2) {
            br[idx] = right_in[i];
        }
    }
    jack->audio->modified = true;

    pthread_mutex_unlock(&jack->audio->mutex);
    return 0;
}

void jack_shutdown(void *arg);

bool configure(struct jack_input* jack) {

    jack_set_process_callback(jack->client, process, jack);
    jack_on_shutdown(jack->client, jack_shutdown, jack);

    jack->audio->rate = jack_get_sample_rate(jack->client);
    jack->audio->sample_sz = jack_get_buffer_size(jack->client) * sizeof(float);

    if (jack->verbose) printf("JACK client name: %s\n", jack_get_client_name(jack->client));
    printf("JACK: sample rate/size was overwritten, new values: %i, %i\n",
           (int) jack->audio->rate, (int) jack->audio->sample_sz);

    if (jack->audio->sample_sz / sizeof(float) > jack->audio->audio_buf_sz) {
        fprintf(stderr, "ERROR: audio buffer is too small: %li\n", jack->audio->audio_buf_sz);
        return false;
    }

    if (jack->audio->channels == 1) {
        jack->left_input_port = jack_port_register(jack->client, "Mono",
                                                   JACK_DEFAULT_AUDIO_TYPE,
                                                   JackPortIsInput, 0);
    } else {
        jack->left_input_port = jack_port_register(jack->client, "L",
                                                   JACK_DEFAULT_AUDIO_TYPE,
                                                   JackPortIsInput, 0);
        jack->right_input_port = jack_port_register(jack->client, "R",
                                                    JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsInput, 0);
    }

    if (jack_activate(jack->client)) {
        fprintf(stderr, "Cannot activate JACK client\n");
        return false;
    }
    return true;
}

void  silent_jack_error_callback(const char *msg) {}

void* monitor(void* jack_ptr) {

    struct jack_input* jack = (struct jack_input*)jack_ptr;
    jack_set_error_function(silent_jack_error_callback);
    while (true) {

        pthread_spin_lock(&jack->state_sync);
        if (jack->state == PREPARING_TO_TERMINATE) {
            jack->state = TERMINATING;
            pthread_barrier_wait(&jack->barrier);
            pthread_spin_unlock(&jack->state_sync);
            return 0;
        }
        if (jack->state == TERMINATING) {
            pthread_spin_unlock(&jack->state_sync);
            return 0;
        }
        pthread_spin_unlock(&jack->state_sync);

        jack_status_t status;

        jack->client = jack_client_open("glava", JackNoStartServer, &status);
        if (jack->client != NULL) {
            break;
        }

        if (jack->verbose) fprintf(stderr, "jack_client_open() failed, "
                "status = 0x%2.0x\n", status);
        if (status & JackServerFailed) {
            if (jack->verbose) fprintf(stderr, "Unable to connect to JACK server\n");
        }

        //////////bypass to clear screen///////////
        pthread_mutex_lock(&jack->audio->mutex);
        jack->audio->modified = true;
        pthread_mutex_unlock(&jack->audio->mutex);
        ///////////////////////////////////////////

        /* Sleep for 500ms and then attempt to connect again */
        struct timespec tv = {
            .tv_sec = 0, .tv_nsec = 500 * 1000000
        };
        nanosleep(&tv, NULL);
    }
    jack_set_error_function(NULL);
    configure(jack);
    return 0;
}

void jack_shutdown(void *arg) {

    int return_status;

    struct jack_input* jack = (struct jack_input*) arg;

    pthread_spin_lock(&jack->state_sync);
    if (jack->state == PREPARING_TO_TERMINATE) {
        jack->state = TERMINATING;
        pthread_barrier_wait(&jack->barrier);
        pthread_spin_unlock(&jack->state_sync);
        return;
    }
    if (jack->state == TERMINATING) {
        pthread_spin_unlock(&jack->state_sync);
        return;
    }
    pthread_spin_unlock(&jack->state_sync);

    if (jack->monitoring_thread) {
        if ((return_status = pthread_join(jack->monitoring_thread, NULL))) {
            fprintf(stderr, "Failed to join with audio thread: %s\n", strerror(return_status));
        }
    }
    jack->client = NULL;

    if ((return_status = pthread_create(&jack->monitoring_thread, NULL, &monitor, (void*) jack))) {
        fprintf(stderr, "Failed to create monitoring thread for JACK: %i\n", return_status);
        exit(EXIT_FAILURE);
    }
}

jack_input_ptr init_jack_client(struct audio_data* audio, bool verbose) {

    struct jack_input* jack = malloc(sizeof(struct jack_input));
    jack->audio = audio;
    jack->verbose = verbose;
    jack->right_input_port = NULL;
    jack->state = WORKING;
    jack->monitoring_thread = 0;
    pthread_spin_init(&jack->state_sync, 0);
    pthread_barrier_init(&jack->barrier, NULL, 2);

    jack_status_t status;

    jack->client = jack_client_open("glava", JackNullOption, &status);
    if (jack->client == NULL) {
        fprintf(stderr, "jack_client_open() failed, "
                "status = 0x%2.0x\n", status);
        if (status & JackServerFailed) {
            fprintf(stderr, "Unable to connect to JACK server\n");
        }
        exit(EXIT_FAILURE);
    }
    if (status & JackServerStarted) {
        if (verbose) fprintf(stderr, "JACK server started\n");
    }

    if (!configure(jack)) {
        exit(EXIT_FAILURE);
    }
    return (jack_input_ptr)jack;
}

void close_jack_client(jack_input_ptr jack_ptr) {

    struct jack_input* jack = (struct jack_input*)jack_ptr;

    pthread_spin_lock(&jack->state_sync);
    jack->state = PREPARING_TO_TERMINATE;
    pthread_spin_unlock(&jack->state_sync);

    pthread_barrier_wait(&jack->barrier);

    int return_status;

    if (jack->monitoring_thread) {
        if ((return_status = pthread_join(jack->monitoring_thread, NULL))) {
            fprintf(stderr, "Failed to join with audio thread: %s\n", strerror(return_status));
        }
    }

    if (jack->client) {
        jack_client_close (jack->client);
    }

    pthread_spin_destroy(&jack->state_sync);
    pthread_barrier_destroy(&jack->barrier);
    free(jack);
}
#endif
