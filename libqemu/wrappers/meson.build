libqemu_ss.add(files('memory.c', 'cpu.c', 'iothread.c', 'iothread-job.c',
                     'clock.c', 'gpio.c', 'timer.c',
                     'chardev.c', 'console.c', 'rcu.c', 'error.c', 'virgl.c',
		     'qdev.c', 'gdbserver.c'))
libqemu_ss.add(sdl)
libqemu_ss.add(when: 'CONFIG_PCI_EXPRESS_GENERIC_BRIDGE', if_true: files('gpex.c'))
libqemu_ss.add(when: 'CONFIG_PCI_EXPRESS_GENERIC_BRIDGE', if_false: files('stubs/gpex.c'))

subdir('target')

install_headers('gpio.h', 'timer.h', 'memory.h', 'cpu.h', 'display.h',
                'console.h', 'libqemu.h', 'error.h', 'virgl.h', 'gdbserver.h',
                'arm-generic-timer.h',
                'clock.h', 'iothread-job.h',
                subdir : 'libqemu/libqemu/wrappers')
