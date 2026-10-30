/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* Adapt Moonlight's Opus FL-FR-C-LFE-RL-RR-SL-SR channel mapping to
 * the conventional PulseAudio ALSA layout: FL-FR-RL-RR-C-LFE-SL-SR.
 * Unlike native ALSA, this never depends on CORE_ELEC_AUDIO_MAPPING. */
void pulse_alsa_mapping(unsigned char output[8], const unsigned char input[8],
                        int channels);
