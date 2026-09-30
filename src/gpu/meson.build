src_gpu = ['vt-gpu.c']
gpu_deps = [vantage_core_dep]
gpu_cargs = []

# dma-buf import engine (EGL + GLES). Compiled in whenever the GL stack
# is present; the engine itself degrades gracefully at runtime (no DRM
# render node, no dma_buf import extension, VANTAGE_DMABUF=0).
if dep_egl.found() and dep_glesv2.found()
  src_gpu += ['vt-dmabuf-egl.c']
  gpu_deps += [dep_egl, dep_glesv2]
  gpu_cargs += ['-DVT_HAVE_EGL', '-DVT_HAVE_GLES']
  if dep_gbm.found()
    gpu_deps += [dep_gbm]
    gpu_cargs += ['-DVT_HAVE_GBM']
  endif
endif

lib_vantage_gpu = static_library('vantage-gpu',
  sources : src_gpu,
  include_directories : vantage_includes,
  dependencies : gpu_deps,
  c_args : gpu_cargs,
  pic : true,
)

vantage_gpu_dep = declare_dependency(
  link_with : lib_vantage_gpu,
  include_directories : vantage_includes,
  dependencies : [vantage_core_dep] + gpu_deps,
)
