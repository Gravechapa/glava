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

struct jack_input
{
    jack_port_t *left_input_port;
    jack_port_t *right_input_port;
    jack_client_t *client;
    struct audio_data *audio;
    bool verbose;
};


int process(jack_nframes_t nframes, void *arg) {

    struct jack_input* jack = (struct jack_input*) arg;

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

void jack_shutdown(void *arg) {
    // exit(1);
}

jack_input_ptr init_jack_client(struct audio_data* audio, bool verbose) {

    struct jack_input* jack = malloc(sizeof(struct jack_input));
    jack->audio = audio;
    jack->verbose = verbose;
    jack->right_input_port = NULL;

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

    jack_set_process_callback(jack->client, process, jack);
    jack_on_shutdown(jack->client, jack_shutdown, 0);

    audio->rate = jack_get_sample_rate(jack->client);
    audio->sample_sz = jack_get_buffer_size(jack->client) * 4;

    printf("JACK: sample rate/size was overwritten, new values: %i, %i\n",
           (int) audio->rate, (int) audio->sample_sz);

    if (audio->sample_sz / 4 > audio->audio_buf_sz) {
        printf("ERROR: audio buffer is too small: %li\n", audio->audio_buf_sz);
        exit(EXIT_FAILURE);
    }

    jack->left_input_port = jack_port_register(jack->client, "L",
                                               JACK_DEFAULT_AUDIO_TYPE,
                                               JackPortIsInput, 0);

    if (audio->channels == 2) {
        jack->right_input_port = jack_port_register(jack->client, "R",
                                                    JACK_DEFAULT_AUDIO_TYPE,
                                                    JackPortIsInput, 0);
    }

    if (jack_activate(jack->client)) {
        fprintf(stderr, "Cannot activate jack client\n");
        exit(EXIT_FAILURE);
    }
    return (jack_input_ptr)jack;
}

void close_jack_client(jack_input_ptr jack_ptr) {
    struct jack_input* jack = (struct jack_input*)jack_ptr;
    jack_client_close (jack->client);
    free(jack);
}
#endif
