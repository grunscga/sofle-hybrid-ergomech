# Copyright (c) 2026 Greg Grunschel
# SPDX-License-Identifier: MIT

zephyr_include_directories(include)

target_sources_ifdef(CONFIG_ZMK_INPUT_PROCESSOR_MOUSE_SPEED app PRIVATE
    src/mouse_speed.c
    src/input_processor_mouse_speed.c
)

# The keymap names this behavior on both halves. Only the central runs it.
if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_MOUSE_SPEED app PRIVATE
        src/behavior_mouse_speed.c
    )
endif()
