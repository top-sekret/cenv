project (
  'cenv', 'cpp',

  default_options: [ 'cpp_std=c++11' ],
  license: 'GPL3+',
  version: '0.1.0'
)

cfg = configuration_data ()
cfg.set ('VERSION', meson.project_version ())
config_h = configure_file (
  configuration: cfg,
  input: 'config.h.in',
  output: 'config.h'
)

executable (
  'cenv',

  'cenv.cc',
  config_h
)
