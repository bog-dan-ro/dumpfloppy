project('dumpfloppy', 'c',
  version: '1.1.1',
    default_options: [
    'c_std=gnu99'
  ],

)

cc = meson.get_compiler('c')
cc.check_header('linux/fd.h', required: true)
cc.check_header('linux/fdreg.h', required: true)

common_srcs = ['disk.c', 'disk.h', 'imd.c', 'imd.h', 'show.c', 'show.h', 'util.c', 'util.h']

executable('dumpfloppy', ['dumpfloppy.c', common_srcs], install : true)

executable('imdcat', ['imdcat.c', common_srcs], install : true)
