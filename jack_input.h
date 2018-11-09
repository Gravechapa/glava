#pragma once

typedef void* jack_input_ptr;

jack_input_ptr init_jack_client(struct audio_data* audio, bool verbose);
void close_jack_client(jack_input_ptr jack_ptr);
