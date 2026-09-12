# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Enno Boland <g@s01.de>

project(
    'scrssh',
    'c',
    version: '0.3.0',
    license: 'GPL-2.0-only',
    default_options: [
        'c_std=c11',
        'warning_level=3',
        'buildtype=debugoptimized',
    ],
)

pyminify = find_program('pyminify', required: get_option('minify'))

agent_py = files('agent.py')
if pyminify.found()
    agent_py = custom_target(
        'agent.min.py',
        input: 'agent.py',
        output: 'agent.min.py',
        command: [
            pyminify,
            '--rename-globals', '@INPUT@',
            '-o', '@OUTPUT@',
        ],
    )
endif

agent_h = custom_target(
    'agent.h',
    input: agent_py,
    output: 'agent.h',
    feed: true,
    capture: true,
    command: executable('gen_agent', 'gen_agent.c', native: true),
)

executable(
    'scrssh',
    ['scrssh.c', agent_h],
    dependencies: [
        dependency('sdl3'),
        dependency('libavformat'),
        dependency('libavcodec'),
        dependency('libavutil'),
    ],
    install: true,
)
