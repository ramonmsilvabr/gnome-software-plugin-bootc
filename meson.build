project('gnome-software-bootc-plugin', 'c',
  version: '0.1.0',
  meson_version: '>= 0.58.0',
  default_options: ['c_std=gnu11']
)

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.76.0')
gio_dep = dependency('gio-2.0')
json_glib_dep = dependency('json-glib-1.0')
gnome_software_dep = dependency('gnome-software', version: '>= 44.0')

# Target plugin directory
plugins_dir = gnome_software_dep.get_variable(pkgconfig: 'plugindir')

shared_module('gs_plugin_bootc',
  'gs-plugin-bootc.c',
  dependencies: [
    glib_dep,
    gio_dep,
    json_glib_dep,
    gnome_software_dep
  ],
  install: true,
  install_dir: plugins_dir,
  c_args: [
    '-DG_LOG_USE_STRUCTURED',
    '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_76'
  ]
)
