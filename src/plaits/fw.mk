# Plaits: Voice at its native 48 kHz, rendered 12 frames (plaits::kBlockSize)
# per call. All 24 engines.
CC_SOURCES += \
    $(EURORACK)/plaits/resources.cc \
    $(EURORACK)/plaits/dsp/chords/chord_bank.cc \
    $(EURORACK)/plaits/dsp/engine/additive_engine.cc \
    $(EURORACK)/plaits/dsp/engine/bass_drum_engine.cc \
    $(EURORACK)/plaits/dsp/engine/chord_engine.cc \
    $(EURORACK)/plaits/dsp/engine/fm_engine.cc \
    $(EURORACK)/plaits/dsp/engine/grain_engine.cc \
    $(EURORACK)/plaits/dsp/engine/hi_hat_engine.cc \
    $(EURORACK)/plaits/dsp/engine/modal_engine.cc \
    $(EURORACK)/plaits/dsp/engine/noise_engine.cc \
    $(EURORACK)/plaits/dsp/engine/particle_engine.cc \
    $(EURORACK)/plaits/dsp/engine/snare_drum_engine.cc \
    $(EURORACK)/plaits/dsp/engine/speech_engine.cc \
    $(EURORACK)/plaits/dsp/engine/string_engine.cc \
    $(EURORACK)/plaits/dsp/engine/swarm_engine.cc \
    $(EURORACK)/plaits/dsp/engine/virtual_analog_engine.cc \
    $(EURORACK)/plaits/dsp/engine/waveshaping_engine.cc \
    $(EURORACK)/plaits/dsp/engine/wavetable_engine.cc \
    $(EURORACK)/plaits/dsp/engine2/chiptune_engine.cc \
    $(EURORACK)/plaits/dsp/engine2/phase_distortion_engine.cc \
    $(EURORACK)/plaits/dsp/engine2/six_op_engine.cc \
    $(EURORACK)/plaits/dsp/engine2/string_machine_engine.cc \
    $(EURORACK)/plaits/dsp/engine2/virtual_analog_vcf_engine.cc \
    $(EURORACK)/plaits/dsp/engine2/wave_terrain_engine.cc \
    $(EURORACK)/plaits/dsp/fm/algorithms.cc \
    $(EURORACK)/plaits/dsp/fm/dx_units.cc \
    $(EURORACK)/plaits/dsp/physical_modelling/modal_voice.cc \
    $(EURORACK)/plaits/dsp/physical_modelling/resonator.cc \
    $(EURORACK)/plaits/dsp/physical_modelling/string_voice.cc \
    $(EURORACK)/plaits/dsp/physical_modelling/string.cc \
    $(EURORACK)/plaits/dsp/speech/lpc_speech_synth_controller.cc \
    $(EURORACK)/plaits/dsp/speech/lpc_speech_synth_phonemes.cc \
    $(EURORACK)/plaits/dsp/speech/lpc_speech_synth_words.cc \
    $(EURORACK)/plaits/dsp/speech/lpc_speech_synth.cc \
    $(EURORACK)/plaits/dsp/speech/naive_speech_synth.cc \
    $(EURORACK)/plaits/dsp/speech/sam_speech_synth.cc \
    $(EURORACK)/plaits/dsp/voice.cc

# -Os on the engine: at -O2 the image is 470 KB of the 480 KB SRAM region
# (97.2 %); -Os is 451 KB (93.4 %). Plaits ran at -O2 on a 72 MHz
# Cortex-M4 (STM32F373); the H750 is a 480 MHz M7, so the speed given up is
# affordable -- test/test_plaits.cpp measures it.
FW_CC_OPT := -Os
