project(
    'gnome-software-plugin-android',
    ['c'],
    version: '1.0.0',
    meson_version: '>=0.58'
)

gnome_software_dep = dependency(
  'gnome-software',
  version: '>=50.0',
  fallback: ['gnome-software', 'libgnomesoftware_dep'],
  default_options: [
    'malcontent=false',
    'packagekit=false',
    'webapps=false',
    'fwupd=false',
    'hardcoded_foss_webapps=false',
    'hardcoded_proprietary_webapps=false',
    'external_appstream=true',
    'gtk_doc=false',
    'sysprof=disabled',
  ],
)

arch_triplet = run_command('gcc', '-dumpmachine', check: true).stdout().strip()
plugin_install_dir = gnome_software_dep.get_variable(
  'plugindir',
  default_value: '/usr/lib/@0@/gnome-software/plugins-23'.format(arch_triplet),
)

cargs = [
  '-DG_LOG_DOMAIN="GsPluginAndroid"',
  '-DI_KNOW_THE_GNOME_SOFTWARE_API_IS_SUBJECT_TO_CHANGE',
  '-DGS_PLUGIN_ANDROID_VERSION="@0@"'.format(meson.project_version()),
]

glib_dep = dependency('glib-2.0', version: '>=2.60')
gobject_dep = dependency('gobject-2.0')
gio_dep = dependency('gio-2.0')
appstream_dep = dependency('appstream')
json_glib_dep = dependency('json-glib-1.0')

plugin_android_lib = shared_library(
  'gs_plugin_android',
  sources : 'src/gs-plugin-android/gs-plugin-android.c',
  install : true,
  install_dir: plugin_install_dir,
  c_args : cargs,
  dependencies : [
    gnome_software_dep,
    glib_dep,
    gobject_dep,
    gio_dep,
    appstream_dep,
    json_glib_dep,
  ],
)

install_data(
  'data/org.gnome.Software.Plugin.Android.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo'),
)
