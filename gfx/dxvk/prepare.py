"""Prepare the pinned zlib-licensed DXVK native 1.9.2b (d3d11 + dxgi) for
ARM64 Android and a headless Linux host.

  prepare.py <dxvk-native checkout> <output dir>

Generic ARM64/Android adaptations follow nfsu2-recompiled's
ports/dxvk_android/prepare.py (MIT); the Direct3D 11 parts are this project's.
Every patch asserts its anchor text, so a different DXVK revision fails loudly.
"""
import argparse, hashlib, pathlib, shutil, subprocess

PIN = "c8dc91fabd00cac11d697ccf07426e798393cd40"
ap = argparse.ArgumentParser()
ap.add_argument("source", type=pathlib.Path)
ap.add_argument("output", type=pathlib.Path)
args = ap.parse_args()
rev = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
if rev != PIN:
    raise SystemExit("Unexpected DXVK revision: " + rev)
if args.output.exists():
    shutil.rmtree(args.output)
args.output.mkdir(parents=True)
for d in ("src", "include"):
    shutil.copytree(args.source / d, args.output / d)
shutil.copy2(args.source / "LICENSE", args.output / "LICENSE")
here = pathlib.Path(__file__).parent
nfs = here / ".." / ".." / "third_party" / "dxvk_android_include"
for h in ("vulkan_android.h", "vulkan_xlib.h"):
    shutil.copy2(nfs / h, args.output / "include/vulkan" / h)


def replace(path, old, new, count=1):
    f = args.output / path
    t = f.read_text()
    if t.count(old) < 1:
        raise SystemExit(f"Patch mismatch: {path}: {old[:60]!r}")
    f.write_text(t.replace(old, new, count) if count else t.replace(old, new))


# ---- ARM64: no x86 intrinsics -------------------------------------------
replace("src/util/util_bit.h", "#pragma once", "#pragma once\n#include <cstdint>")
replace("src/util/util_bit.h", "#ifndef _MSC_VER", "#if defined(__i386__) || defined(__x86_64__)\n#ifndef _MSC_VER")
replace("src/util/util_bit.h", '#include "util_likely.h"', '#endif // x86 intrinsics\n#include "util_likely.h"')
replace("src/util/util_bit.h", "#elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;",
        "#elif defined(__aarch64__) || defined(__arm__)\n    return n ? __builtin_ctz(n) : 32;\n    #elif defined(__GNUC__) || defined(__clang__)\n    uint32_t res;")
replace("src/util/util_bit.h", "#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)", "#if defined(__i386__) || defined(__x86_64__)")
replace("src/util/sync/sync_spinlock.h", "_mm_pause();",
        '#if defined(__aarch64__) || defined(__arm__)\n        asm volatile("yield");\n#else\n        _mm_pause();\n#endif')
replace("src/util/util_vector.h", "    __m128 value = _mm_load_ps(a.data);",
        "#if defined(__aarch64__) || defined(__arm__)\n    for (unsigned i = 0; i < 4; ++i) result.data[i] = a.data[i] == a.data[i] ? a.data[i] : 0.0f;\n#else\n    __m128 value = _mm_load_ps(a.data);")
replace("src/util/util_vector.h", "    _mm_store_ps(result.data, value);", "    _mm_store_ps(result.data, value);\n#endif")

# ---- logging: everything through one host hook (logcat / runtime log) ----
replace("src/util/log/log.cpp", '#include "log.h"\n',
        '#include "log.h"\nextern "C" __attribute__((weak)) void hp_dxvk_log(int level, const char* line);\n')
replace("src/util/log/log.cpp", "        std::cerr << prefix << line << std::endl;\n",
        "        std::cerr << prefix << line << std::endl;\n        if (hp_dxvk_log) hp_dxvk_log(int(level), line.c_str());\n")

# ---- Android presents landscape images as SUBOPTIMAL every frame ----------
replace("src/d3d11/d3d11_swapchain.cpp",
        "    if (status != VK_SUCCESS)\n      RecreateSwapChain(m_vsync);",
        "    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n      RecreateSwapChain(m_vsync);")

# ---- d3d11 and dxgi live in one library here: one logger instance ---------
replace("src/dxgi/dxgi_main.cpp", '  Logger Logger::s_instance("dxgi.log");\n', '  // logger instance: see d3d11_main.cpp (single library)\n')

# DXVK caps allocations at 80% of the reported heap on unified-memory devices.
# That presumes a real heap size; lavapipe (host tests) reports a fixed 2 GB,
# less than the game uses for one track. HP_DXVK_NO_HEAP_BUDGET=1 lifts the cap.
replace("src/dxvk/dxvk_memory.cpp",
        "        m_memHeaps[i].budget = (8 * m_memProps.memoryHeaps[i].size) / 10;",
        "        m_memHeaps[i].budget = env::getEnvVar(\"HP_DXVK_NO_HEAP_BUDGET\").empty() ? (8 * m_memProps.memoryHeaps[i].size) / 10 : 0;")

# Qualcomm's proprietary driver (seen on Adreno 830, driver 512.800.72): a fragment
# shader that declares the SignedZeroInfNanPreserve execution mode
# (VK_KHR_shader_float_controls) reads gl_FragCoord as (0,0). Every full-screen
# pass of the engine derives its texture coordinates from SV_Position, so the
# whole post-processing chain collapsed to one texel. d3d11.floatControls
# therefore defaults to off on that driver (a dxvk.conf entry still overrides).
replace("src/d3d11/d3d11_options.cpp",
        "    this->floatControls         = config.getOption<bool>(\"d3d11.floatControls\", true);",
        "    this->floatControls         = config.getOption<bool>(\"d3d11.floatControls\",\n"
        "      devInfo.khrDeviceDriverProperties.driverID != VK_DRIVER_ID_QUALCOMM_PROPRIETARY_KHR);")
# ---- implicit flush at render-target changes under the bridge's control ----------
replace("src/d3d11/d3d11_context_imm.cpp", '          ID3D11DepthStencilView*           pDepthStencilView) {\n    FlushImplicit(TRUE);\n    \n    D3D11DeviceContext::OMSetRenderTargets(', '          ID3D11DepthStencilView*           pDepthStencilView) {\n    if (int m = hp_rp_mode()) { if (m == 2) Flush(); else TmBarrier(); } else if (hp_rt_change_flush()) FlushImplicit(TRUE);\n    \n    D3D11DeviceContext::OMSetRenderTargets(')
replace("src/d3d11/d3d11_context_imm.cpp", '    const UINT*                             pUAVInitialCounts) {\n    FlushImplicit(TRUE);\n\n    D3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(', '    const UINT*                             pUAVInitialCounts) {\n    if (int m = hp_rp_mode()) { if (m == 2) Flush(); else TmBarrier(); } else if (hp_rt_change_flush()) FlushImplicit(TRUE);\n\n    D3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(')
replace("src/d3d11/d3d11_context_imm.cpp", 'namespace dxvk {\n  \n  D3D11ImmediateContext::D3D11ImmediateContext(', '// tm: the bridge places the submission boundaries between render passes itself (rp_split\n// in gfx/d3d11_bridge.cpp), because on Adreno a boundary between certain passes decides\n// whether a frame comes out right. DXVK\'s timer-driven flush at a render-target change\n// would add one wherever 750 us happen to have passed. g_hp_dxvk_implicit_flush = 0 turns\n// that one off (the other implicit flushes -- query polls, busy resources -- stay);\n// HP_DXVK_IMPLICIT_FLUSH sets it, the bridge chooses the default.\nextern "C" { int g_hp_dxvk_implicit_flush = -1; int hp_rp_mode(void); }\nstatic inline bool hp_rt_change_flush() {\n  if (g_hp_dxvk_implicit_flush < 0) {\n    const char* e = getenv("HP_DXVK_IMPLICIT_FLUSH");\n    g_hp_dxvk_implicit_flush = e && *e ? atoi(e) : 1;\n  }\n  return g_hp_dxvk_implicit_flush != 0;\n}\n\nnamespace dxvk {\n  \n  D3D11ImmediateContext::D3D11ImmediateContext(')
replace("src/d3d11/d3d11_context_imm.cpp", '#include "d3d11_cmdlist.h"', '#include <cstdlib>\n#include "d3d11_cmdlist.h"')

# ---- Command buffers in flight ---------------------------------------------
# The bridge splits render passes with a Flush (one submission each) where Adreno needs it
# (gfx/d3d11_bridge.cpp, rp_split): ~40 submissions per frame in a race. DXVK 1.9 lets only
# 18 command buffers be submitted-but-unfinished; beyond that the CS thread waits for the
# GPU, so the next frame could not be recorded while the GPU drew this one and CPU and GPU
# ran one after the other (frame time = CPU + GPU, "in Present" ~ GPU time). 96 covers
# about two frames; the frame latency limit (3 frames) still bounds how far ahead the CPU runs.
replace("src/dxvk/dxvk_limits.h", "    MaxNumQueuedCommandBuffers  =    18,", "    MaxNumQueuedCommandBuffers  =    96,")

# ---- Render-pass split without a submission (TM_RP_BARRIER) --------------------
# The bridge's Adreno render-pass splits (rp_split) were Flushes: ~40 submissions per frame,
# costing CPU time and about a third of the GPU time. hp_dxvk_barrier() ends the render
# pass and records a full memory barrier instead, in the same command buffer.
replace("src/dxvk/dxvk_context.h", "    void flushCommandList();\n",
        "    void flushCommandList();\n\n    /** tm: end the render pass, full memory barrier (render-pass split without a submission) */\n    void tmFullBarrier();\n")
replace("src/dxvk/dxvk_context.cpp", "  void DxvkContext::beginQuery(const Rc<DxvkGpuQuery>& query) {",
        "  void DxvkContext::tmFullBarrier() {\n"
        "    this->spillRenderPass(true);\n"
        "    m_execBarriers.recordCommands(m_cmd);\n"
        "    this->emitMemoryBarrier(0,\n"
        "      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,\n"
        "      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);\n"
        "  }\n\n\n"
        "  void DxvkContext::beginQuery(const Rc<DxvkGpuQuery>& query) {")
replace("src/d3d11/d3d11_context_imm.h", "    void STDMETHODCALLTYPE Flush();\n",
        "    void STDMETHODCALLTYPE Flush();\n\n    void TmBarrier();\n")
replace("src/d3d11/d3d11_context_imm.cpp", "  void STDMETHODCALLTYPE D3D11ImmediateContext::Flush() {",
        "  void D3D11ImmediateContext::TmBarrier() {\n"
        "    D3D10DeviceLock lock = LockContext();\n"
        "    EmitCs([] (DxvkContext* ctx) { ctx->tmFullBarrier(); });\n"
        "  }\n\n\n"
        "  void STDMETHODCALLTYPE D3D11ImmediateContext::Flush() {")
f = args.output / "src/d3d11/d3d11_context_imm.cpp"
f.write_text(f.read_text() + '\n// tm: called by the bridge (gfx/d3d11_bridge.cpp, rp_split)\n'
             'extern "C" void hp_dxvk_barrier(ID3D11DeviceContext* c) {\n'
             '  if (c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE) static_cast<dxvk::D3D11ImmediateContext*>(c)->TmBarrier();\n'
             '  else c->Flush();\n'
             '}\n')

# ---- Feature levels on smaller Adreno GPUs --------------------------------
# DXVK 1.9 asks for a set of Vulkan features per D3D feature level and offers the highest
# level whose set the GPU has. The Adreno 710 driver of the iPlay60 mini Turbo (driver
# 0615.84) lacks some of the 9_3..11_0 ones, so the game got level 9_2, took its low-end
# render path and crashed in it (0x007D1F40). Trackmania does not use stream output,
# multiple viewports or the other "nice to have" features below, so they are enabled when
# present and no longer decide the level. GPUs that have them all (Odin3, Adreno 830)
# behave exactly as before. The ones a GPU lacks are logged once.
D = "src/d3d11/d3d11_device.cpp"
import re
for feat in ("independentBlend", "multiViewport", "shaderImageGatherExtended", "dualSrcBlend", "imageCubeArray",
             "drawIndirectFirstInstance", "fragmentStoresAndAtomics", "multiDrawIndirect", "tessellationShader"):
    m = re.search(r"\n( +enabled\.core\.features\.%s +)= VK_TRUE;" % feat, (args.output / D).read_text())
    if not m:
        raise SystemExit("Patch mismatch: %s: %s" % (D, feat))
    replace(D, m.group(1) + "= VK_TRUE;", m.group(1) + "= supported.core.features.%s;" % feat)
replace(D, "      enabled.extTransformFeedback.transformFeedback              = VK_TRUE;\n      enabled.extTransformFeedback.geometryStreams                = VK_TRUE;",
        "      enabled.extTransformFeedback.transformFeedback              = supported.extTransformFeedback.transformFeedback;\n"
        "      enabled.extTransformFeedback.geometryStreams                = supported.extTransformFeedback.geometryStreams;")
replace(D, "      enabled.core.features.vertexPipelineStoresAndAtomics        = VK_TRUE;\n    }\n    \n    return enabled;",
        "      enabled.core.features.vertexPipelineStoresAndAtomics        = VK_TRUE;\n    }\n    \n"
        "    static bool tmLogged = false;\n"
        "    if (!tmLogged && featureLevel >= D3D_FEATURE_LEVEL_11_0) {\n"
        "      tmLogged = true;\n"
        "      std::string m;\n"
        "      const auto& f = supported.core.features;\n"
        "      if (!f.independentBlend) m += \" independentBlend\";\n"
        "      if (!f.multiViewport) m += \" multiViewport\";\n"
        "      if (!f.shaderImageGatherExtended) m += \" shaderImageGatherExtended\";\n"
        "      if (!f.dualSrcBlend) m += \" dualSrcBlend\";\n"
        "      if (!f.imageCubeArray) m += \" imageCubeArray\";\n"
        "      if (!f.drawIndirectFirstInstance) m += \" drawIndirectFirstInstance\";\n"
        "      if (!f.fragmentStoresAndAtomics) m += \" fragmentStoresAndAtomics\";\n"
        "      if (!f.multiDrawIndirect) m += \" multiDrawIndirect\";\n"
        "      if (!f.tessellationShader) m += \" tessellationShader\";\n"
        "      if (!supported.extTransformFeedback.transformFeedback) m += \" transformFeedback\";\n"
        "      if (!m.empty()) Logger::warn(str::format(\"tm: GPU lacks optional features:\", m));\n"
        "    }\n"
        "    return enabled;")

# ---- House Party: Win32 waitable objects through the recompiled runtime ---------------------
# DXVK native has no Win32 events, so the frame latency waitable object (Unity's flip-model
# swap chain asks for one) threw, and ID3D11DeviceContext3::Flush1 never signalled its event.
# The runtime provides them as guest handles (runtime/src/k32_sync.c, hp_dxvk_* hooks).
HOOKS = ('#include <cstdint>\n'
         'extern "C" void* hp_dxvk_sem_create(uint32_t initial, uint32_t max);\n'
         'extern "C" void hp_dxvk_sem_release(void* h);\n'
         'extern "C" void hp_dxvk_event_set(void* h);\n'
         'extern "C" void hp_dxvk_handle_close(void* h);\n')
SW = "src/d3d11/d3d11_swapchain.cpp"
replace(SW, '#include "d3d11_swapchain.h"\n', '#include "d3d11_swapchain.h"\n' + HOOKS, 1)
replace(SW, '      throw DxvkError("DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT not supported on this platform.");',
        '      if (const char* hpMin = getenv("HP_FRAME_LATENCY")) m_frameLatency = std::max<uint32_t>(m_frameLatency, uint32_t(std::atoi(hpMin)));\n'
        '      m_frameLatencyEvent = hp_dxvk_sem_create(m_frameLatency ? m_frameLatency : 1, 16);')
replace(SW, '  void D3D11SwapChain::DestroyFrameLatencyEvent() {\n#ifndef DXVK_NATIVE\n    CloseHandle(m_frameLatencyEvent);\n#endif',
        '  void D3D11SwapChain::DestroyFrameLatencyEvent() {\n#ifndef DXVK_NATIVE\n    CloseHandle(m_frameLatencyEvent);\n#else\n'
        '    if (m_frameLatencyEvent) hp_dxvk_handle_close(m_frameLatencyEvent);\n#endif')
replace(SW, '        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);\n      });\n    }\n#endif',
        '        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);\n      });\n    }\n#else\n'
        '    if (m_frameLatencyEvent) {\n'
        '      m_frameLatencySignal->setCallback(m_frameId, [cFrameLatencyEvent = m_frameLatencyEvent] () {\n'
        '        hp_dxvk_sem_release(cFrameLatencyEvent);\n'
        '      });\n'
        '    }\n#endif')
CI = "src/d3d11/d3d11_context_imm.cpp"
replace(CI, '#include <cstdlib>\n', '#include <cstdlib>\n' + HOOKS, 1)
replace(CI, '  void D3D11ImmediateContext::SignalEvent(HANDLE hEvent) {\n#ifndef DXVK_NATIVE',
        '  void D3D11ImmediateContext::SignalEvent(HANDLE hEvent) {\n#ifdef DXVK_NATIVE\n#define SetEvent(h) hp_dxvk_event_set(h)\n#endif\n#if 1')

# ---- HP_DXVK_ADRENO_SIM=1: a desktop GPU reports what the Adreno 730 lacks ----------------
# Host test aid (lavapipe): without these the game takes other paths than on the device.
AD = "src/dxvk/dxvk_adapter.cpp"
replace(AD, "    m_deviceExtensions = DxvkNameSet::enumDeviceExtensions(m_vki, m_handle);",
        "    m_deviceExtensions = DxvkNameSet::enumDeviceExtensions(m_vki, m_handle);\n"
        "    if (!env::getEnvVar(\"HP_DXVK_ADRENO_SIM\").empty()) {\n"
        "      DxvkExt a = { VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME, DxvkExtMode::Optional };\n"
        "      DxvkExt b = { VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, DxvkExtMode::Optional };\n"
        "      DxvkExt c = { VK_EXT_4444_FORMATS_EXTENSION_NAME, DxvkExtMode::Optional };\n"
        "      m_deviceExtensions.disableExtension(a); m_deviceExtensions.disableExtension(b); m_deviceExtensions.disableExtension(c);\n"
        "    }")
replace(AD, "    this->queryDeviceFeatures();\n    this->queryDeviceQueues();",
        "    this->queryDeviceFeatures();\n"
        "    if (!env::getEnvVar(\"HP_DXVK_ADRENO_SIM\").empty()) {\n"
        "      auto& f = m_deviceFeatures.core.features;\n"
        "      f.multiViewport = VK_FALSE; f.shaderFloat64 = VK_FALSE; f.shaderInt64 = VK_FALSE;\n"
        "      Logger::warn(\"HP_DXVK_ADRENO_SIM: no multiViewport, float64/int64, viewport index layer, depth clip enable, 4444\");\n"
        "    }\n"
        "    this->queryDeviceQueues();")

# ---- HP_DXVK_EDS=0: without VK_EXT_extended_dynamic_state (dynamic vertex strides) -----------
# A debugging switch: some Qualcomm drivers are reported to fetch vertices with wrong dynamic
# strides. It was not the cause of the Tab S8 mosaic (odd swap chain height, HouseActivity).
replace(AD, "    this->queryDeviceFeatures();\n    if (!env::getEnvVar(\"HP_DXVK_ADRENO_SIM\").empty()) {",
        "    {\n"
        "      std::string eds = env::getEnvVar(\"HP_DXVK_EDS\");\n"
                "      if (eds == \"0\") {\n"
        "        DxvkExt e = { VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, DxvkExtMode::Optional };\n"
        "        m_deviceExtensions.disableExtension(e);\n"
        "        Logger::info(\"VK_EXT_extended_dynamic_state disabled (HP_DXVK_EDS=0)\");\n"
        "      }\n"
        "    }\n"
        "    this->queryDeviceFeatures();\n    if (!env::getEnvVar(\"HP_DXVK_ADRENO_SIM\").empty()) {")

# ---- free empty memory chunks ----------------------------------------------------------------
# DXVK 1.9 never returns a 128 MB chunk to the driver once allocated, so loading peaks (staging
# buffers for texture uploads) stay resident: House Party's menu held 2.8 GB of GPU memory for
# 1.25 GB in use, and Android killed the game while it loaded the house on an 8 GB tablet.
# Empty chunks are freed now, keeping one spare per memory type.
MH = "src/dxvk/dxvk_memory.h"
replace(MH, "    void free(\n            VkDeviceSize  offset,\n            VkDeviceSize  length);\n    \n  private:\n    \n    struct FreeSlice {",
        "    void free(\n            VkDeviceSize  offset,\n            VkDeviceSize  length);\n\n"
        "    /** hp: true when nothing in the chunk is allocated */\n"
        "    bool isEmpty() const {\n"
        "      return m_freeList.size() == 1 && m_freeList[0].offset == 0 && m_freeList[0].length == m_memory.memSize;\n"
        "    }\n    \n  private:\n    \n    struct FreeSlice {")
MC = "src/dxvk/dxvk_memory.cpp"
replace(MC, "          VkDeviceSize          length) {\n    chunk->free(offset, length);\n  }",
        "          VkDeviceSize          length) {\n    chunk->free(offset, length);\n\n"
        "    if (chunk->isEmpty()) {\n"
        "      uint32_t empty = 0;\n"
        "      for (const auto& c : type->chunks)\n"
        "        empty += c->isEmpty() ? 1 : 0;\n"
        "      if (empty > 1) {\n"
        "        for (auto i = type->chunks.begin(); i != type->chunks.end(); i++) {\n"
        "          if (i->ptr() == chunk) { type->chunks.erase(i); break; }   // the destructor frees the device memory\n"
        "        }\n"
        "      }\n"
        "    }\n  }")

# ---- HP_MEMLOG: device memory allocations (sizes, memory type, heap totals) ---------------------
replace(MC, "namespace dxvk {\n", "namespace dxvk {\n\n"
        "  // hp: HP_MEMLOG=1 logs device memory allocations of 8 MB and more and every 64 MB step of a heap\n"
        "  static void hpMemLog(const char* op, uint32_t typeId, VkDeviceSize size, bool dedicated, VkDeviceSize heapTotal) {\n"
        "    static int on = -1;\n"
        "    if (on < 0) on = env::getEnvVar(\"HP_MEMLOG\").empty() ? 0 : 1;\n"
        "    static VkDeviceSize lastStep = 0;\n"
        "    VkDeviceSize step = heapTotal >> 26;\n"
        "    if (!on || (size < (8u << 20) && step == lastStep)) return;\n"
        "    lastStep = step;\n"
        "    Logger::warn(str::format(\"hp mem \", op, \" type \", typeId, dedicated ? \" dedicated \" : \" \", size >> 10, \" KB, heap now \", heapTotal >> 20, \" MB\"));\n"
        "  }\n", 1)
replace(MC, "    type->heap->stats.memoryAllocated += size;\n    m_device->adapter()->notifyHeapMemoryAlloc(type->heapId, size);\n",
        "    type->heap->stats.memoryAllocated += size;\n    m_device->adapter()->notifyHeapMemoryAlloc(type->heapId, size);\n"
        "    hpMemLog(\"alloc\", type->memTypeId, size, dedAllocInfo != nullptr, type->heap->stats.memoryAllocated);\n")
replace(MC, "    type->heap->stats.memoryAllocated -= memory.memSize;\n    m_device->adapter()->notifyHeapMemoryFree(type->heapId, memory.memSize);\n",
        "    type->heap->stats.memoryAllocated -= memory.memSize;\n    m_device->adapter()->notifyHeapMemoryFree(type->heapId, memory.memSize);\n"
        "    hpMemLog(\"free\", type->memTypeId, memory.memSize, false, type->heap->stats.memoryAllocated);\n")

# ---- smaller memory chunks ----------------------------------------------------------------------
# 128 MB chunks per memory type (and per priority) cost hundreds of MB on a phone where GPU memory
# is system memory: a D3D11 device that Unity only creates to probe the GPU held 768 MB. Chunks
# are 32 MB now (HP_DXVK_CHUNK_MB overrides); bigger allocations get their own device memory.
replace(MC, "#include \"dxvk_device.h\"\n", "#include <cstdlib>\n\n#include \"dxvk_device.h\"\n", 1)
replace(MC, "    // Default to a chunk size of 128 MiB\n    VkDeviceSize chunkSize = 128 << 20;\n",
        "    // hp: 32 MiB chunks (HP_DXVK_CHUNK_MB)\n    VkDeviceSize chunkSize = 32 << 20;\n"
        "    std::string hpChunk = env::getEnvVar(\"HP_DXVK_CHUNK_MB\");\n"
        "    if (!hpChunk.empty() && std::atoi(hpChunk.c_str()) > 0)\n"
        "      chunkSize = VkDeviceSize(std::atoi(hpChunk.c_str())) << 20;\n")
replace(MC, "  DxvkMemoryAllocator::~DxvkMemoryAllocator() {\n    \n  }",
        "  DxvkMemoryAllocator::~DxvkMemoryAllocator() {\n"
        "    if (!env::getEnvVar(\"HP_MEMLOG\").empty())\n"
        "      Logger::warn(\"hp mem allocator destroyed\");\n  }")

# Device-local resources of a quarter chunk and more get their own device memory: freed render
# targets and shadow maps would otherwise leave holes in the chunks that smaller allocations rarely
# fill. Host-visible memory (staging, dynamic buffers - short-lived, every frame) stays in chunks:
# a vkAllocateMemory per upload cost ~5 % of the main thread on Adreno (kgsl ioctls).
replace(MC, "    if (size >= type->chunkSize || dedAllocInfo) {\n      DxvkDeviceMemory devMem = this->tryAllocDeviceMemory(",
        "    if (size >= (isDeviceLocal && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? type->chunkSize / 4 : type->chunkSize) || dedAllocInfo) {\n      DxvkDeviceMemory devMem = this->tryAllocDeviceMemory(")

# ---- GetFrameStatistics: a real SyncQPCTime ------------------------------------------------------
# DXVK native fills SyncQPCTime only on Windows. Unity derives its unscaled clock from it: with the
# caller's uninitialised stack value Time.unscaledTime froze (43080.6 s) and unscaledDeltaTime sat at
# its 1e-5 minimum, so every UI pop-in animation (settings, dialogue, game menu) stayed at ~0 scale.
# The guest's QueryPerformanceCounter clock (runtime) is used now; the refresh counts follow the
# present count (one refresh per present).
DS = "src/dxgi/dxgi_swapchain.cpp"
replace(DS, '#include "dxgi_swapchain.h"\n', '#include "dxgi_swapchain.h"\n\nextern "C" uint64_t w64_qpc(void);\n', 1)
replace(DS, "    pStats->PresentRefreshCount = 0;\n    pStats->SyncRefreshCount = 0;\n#ifdef _WIN32\n    QueryPerformanceCounter(&pStats->SyncQPCTime);\n#endif\n",
        "    pStats->PresentRefreshCount = m_presentCount;\n    pStats->SyncRefreshCount = m_presentCount;\n#ifdef _WIN32\n    QueryPerformanceCounter(&pStats->SyncQPCTime);\n#else\n"
        "    pStats->SyncQPCTime.QuadPart = int64_t(w64_qpc());\n#endif\n")

# ---- SetMaximumFrameLatency on the waitable object --------------------------------------------
# DXVK native raised the waitable semaphore only on Windows: when Unity asked for 2 queued frames
# after creating the swap chain the guest still waited for every frame to be presented before
# starting the next (CPU and GPU each ~30 % busy on the Odin 3). The runtime semaphore is raised
# now; HP_FRAME_LATENCY sets a minimum.
replace(SW, "#ifdef _WIN32\n    if (m_frameLatencyEvent) {\n      // Windows DXGI does not seem to handle the case where the new maximum\n",
        "    if (const char* hpMin = getenv(\"HP_FRAME_LATENCY\"))\n      MaxLatency = std::max<UINT>(MaxLatency, UINT(std::atoi(hpMin)));\n"
        "    Logger::warn(str::format(\"hp: SetMaximumFrameLatency \", MaxLatency, \" (was \", m_frameLatency, \")\"));\n"
        "#ifndef _WIN32\n    if (m_frameLatencyEvent && MaxLatency > m_frameLatency) {\n"
        "      for (UINT i = m_frameLatency; i < MaxLatency; i++)\n        hp_dxvk_sem_release(m_frameLatencyEvent);\n    }\n#endif\n"
        "#ifdef _WIN32\n    if (m_frameLatencyEvent) {\n      // Windows DXGI does not seem to handle the case where the new maximum\n")

# ---- Android background/resume: recreate VkSurfaceKHR when ANativeWindow changes ----------------
# On Android, leaving the app destroys the ANativeWindow and returning provides a new one. DXVK 1.9
# only recreates m_surface when vkGetPhysicalDeviceSurfaceCapabilitiesKHR returns SURFACE_LOST,
# which Adreno does not reliably return before vkCreateSwapchainKHR fails (killing Unity on resume).
PH = "src/vulkan/vulkan_presenter.h"
PC = "src/vulkan/vulkan_presenter.cpp"
replace(PH, "    bool hasSwapChain() const {\n      return m_swapchain;\n    }\n",
        "    bool hasSwapChain() const {\n      return m_swapchain;\n    }\n\n"
        "    bool surfaceStale() const;\n")
replace(PH, "    uint32_t m_imageIndex = 0;\n",
        "    uint64_t m_surfaceGen = 0;\n    uint32_t m_imageIndex = 0;\n")
replace(PC, '#include "../wsi/wsi_presenter.h"\n',
        '#include "../wsi/wsi_presenter.h"\n\n'
        'extern "C" __attribute__((weak)) uint64_t hp_wsi_surface_gen(void);\n')
replace(PC, "  PresenterInfo Presenter::info() const {\n",
        "  bool Presenter::surfaceStale() const {\n"
        "    return hp_wsi_surface_gen && m_surfaceGen != hp_wsi_surface_gen();\n"
        "  }\n\n"
        "  PresenterInfo Presenter::info() const {\n")
replace(PC, "    if (m_swapchain)\n      destroySwapchain();\n",
        "    if (m_swapchain)\n      destroySwapchain();\n\n"
        "#ifdef __ANDROID__\n"
        "    if (m_surface)\n"
        "      destroySurface();\n"
        "    {\n"
        "      VkResult sr = createSurface();\n"
        "      if (sr != VK_SUCCESS) {\n"
        "        m_info.imageCount = 0;\n"
        "        m_info.format     = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };\n"
        "        return sr == VK_ERROR_SURFACE_LOST_KHR ? VK_SUCCESS : sr;\n"
        "      }\n"
        "      if (hp_wsi_surface_gen)\n"
        "        m_surfaceGen = hp_wsi_surface_gen();\n"
        "    }\n"
        "#endif\n")
replace(PC, "    if ((status = m_vkd->vkCreateSwapchainKHR(m_vkd->device(),\n        &swapInfo, nullptr, &m_swapchain)) != VK_SUCCESS)\n      return status;\n",
        "    if ((status = m_vkd->vkCreateSwapchainKHR(m_vkd->device(),\n        &swapInfo, nullptr, &m_swapchain)) != VK_SUCCESS) {\n"
        "#ifdef __ANDROID__\n"
        "      if (status == VK_ERROR_SURFACE_LOST_KHR || status == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {\n"
        "        m_info.imageCount = 0;\n"
        "        m_info.format     = { VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };\n"
        "        return VK_SUCCESS;\n"
        "      }\n"
        "#endif\n"
        "      return status;\n"
        "    }\n")
replace(SW, "    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)\n      RecreateSwapChain(m_vsync);",
        "    if ((status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) || !m_presenter->hasSwapChain() || m_presenter->surfaceStale())\n      RecreateSwapChain(m_vsync);")

CH = "src/d3d11/d3d11_context_imm.h"
replace(CH, '#ifndef DXVK_NATIVE\n#include "../util/sync/sync_signal.h"\n#endif', '#include "../util/sync/sync_signal.h"')
replace(CH, '#ifndef DXVK_NATIVE\n    Rc<sync::CallbackFence> m_eventSignal;\n#endif', '    Rc<sync::CallbackFence> m_eventSignal;')

# ---- HP_STUTTER: lightweight pipeline compile timing hook (no-op when HP_STUTTER unset) --------
for _pf, _is_c in (("src/dxvk/dxvk_graphics.cpp", "0"), ("src/dxvk/dxvk_compute.cpp", "1")):
    replace(_pf, "namespace dxvk {\n", 'extern "C" __attribute__((weak)) void hp_stutter_add_pipe(int is_compute, uint64_t us);\n\nnamespace dxvk {\n', 1)
    replace(_pf,
            "    // Time pipeline compilation for debugging purposes\n"
            "    dxvk::high_resolution_clock::time_point t0, t1;\n\n"
            "    if (Logger::logLevel() <= LogLevel::Debug)\n"
            "      t0 = dxvk::high_resolution_clock::now();\n",
            "    // Time pipeline compilation for debugging purposes\n"
            "    dxvk::high_resolution_clock::time_point t0 = dxvk::high_resolution_clock::now(), t1;\n", 1)
    replace(_pf,
            "    if (Logger::logLevel() <= LogLevel::Debug) {\n"
            "      t1 = dxvk::high_resolution_clock::now();\n",
            "    t1 = dxvk::high_resolution_clock::now();\n"
            "    if (hp_stutter_add_pipe) {\n"
            "      auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();\n"
            f"      hp_stutter_add_pipe({_is_c}, uint64_t(us > 0 ? us : 0));\n"
            "    }\n"
            "    if (Logger::logLevel() <= LogLevel::Debug) {\n", 1)

# ---- Persistent Vulkan pipeline cache (VkPipelineCache -> vk_pipeline.cache) --------------------
# DXVK 1.9 creates an empty VkPipelineCache every launch and never persists Adreno's compiled
# binary shaders to disk, so every launch recompiles all SPIR-V pipelines from scratch.
PCH = "src/dxvk/dxvk_pipecache.h"
PCC = "src/dxvk/dxvk_pipecache.cpp"
replace(PCH,
        "#include <fstream>\n",
        "#include <fstream>\n#include <mutex>\n")
replace(PCH,
        "    VkPipelineCache handle() const {\n      return m_handle;\n    }\n",
        "    VkPipelineCache handle() const {\n      return m_handle;\n    }\n\n"
        "    void noteCompiled();\n"
        "    void saveToDisk();\n")
replace(PCH,
        "    Rc<vk::DeviceFn>        m_vkd;\n    VkPipelineCache         m_handle;\n",
        "    Rc<vk::DeviceFn>        m_vkd;\n    VkPipelineCache         m_handle;\n"
        "    std::string             m_path;\n"
        "    std::mutex              m_saveMu;\n"
        "    std::atomic<uint32_t>   m_dirty{0};\n"
        "    std::atomic<bool>       m_anyCompiled{false};\n"
        "    std::atomic<int64_t>    m_lastSaveMs{0};\n"
        "    std::atomic<bool>       m_saving{false};\n")
replace(PCC,
        '#include "dxvk_pipecache.h"\n\nnamespace dxvk {',
        '#include "dxvk_pipecache.h"\n'
        '#include <cstdio>\n#include <mutex>\n#include <thread>\n#include <vector>\n\n'
        'namespace dxvk {\n'
        '  static std::mutex g_hpPipeCacheMu;\n'
        '  static DxvkPipelineCache* g_hpPipeCache = nullptr;\n'
        '}\n'
        'extern "C" void hp_dxvk_save_pipeline_cache(void) {\n'
        '  std::lock_guard<std::mutex> l(dxvk::g_hpPipeCacheMu);\n'
        '  if (dxvk::g_hpPipeCache) dxvk::g_hpPipeCache->saveToDisk();\n'
        '}\n\n'
        'namespace dxvk {')
replace(PCC,
        "    info.initialDataSize  = 0;\n    info.pInitialData     = nullptr;\n    \n"
        "    if (m_vkd->vkCreatePipelineCache(m_vkd->device(),\n        &info, nullptr, &m_handle) != VK_SUCCESS)\n"
        "      throw DxvkError(\"DxvkPipelineCache: Failed to create cache\");\n  }",
        "    std::vector<char> blob;\n"
        "    std::string dir = env::getEnvVar(\"DXVK_STATE_CACHE_PATH\");\n"
        "    if (!dir.empty()) {\n"
        "      m_path = dir + (*dir.rbegin() == '/' ? \"\" : \"/\") + \"vk_pipeline.cache\";\n"
        "      std::ifstream in(m_path, std::ios::binary | std::ios::ate);\n"
        "      if (in) {\n"
        "        auto sz = in.tellg();\n"
        "        if (sz > 0 && sz <= (64 << 20)) {\n"
        "          blob.resize(size_t(sz));\n"
        "          in.seekg(0);\n"
        "          if (!in.read(blob.data(), sz)) blob.clear();\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "    info.initialDataSize  = blob.size();\n"
        "    info.pInitialData     = blob.empty() ? nullptr : blob.data();\n"
        "    if (m_vkd->vkCreatePipelineCache(m_vkd->device(),\n        &info, nullptr, &m_handle) != VK_SUCCESS) {\n"
        "      info.initialDataSize = 0; info.pInitialData = nullptr;\n"
        "      if (m_vkd->vkCreatePipelineCache(m_vkd->device(),\n          &info, nullptr, &m_handle) != VK_SUCCESS)\n"
        "        throw DxvkError(\"DxvkPipelineCache: Failed to create cache\");\n"
        "    } else if (!blob.empty()) {\n"
        "      Logger::warn(str::format(\"hp: loaded VkPipelineCache (\", blob.size() >> 10, \" KB)\"));\n"
        "    }\n"
        "    std::lock_guard<std::mutex> l(g_hpPipeCacheMu);\n"
        "    g_hpPipeCache = this;\n"
        "  }\n\n"
        "  void DxvkPipelineCache::noteCompiled() {\n"
        "    if (m_path.empty()) return;\n"
        "    m_anyCompiled.store(true, std::memory_order_relaxed);\n"
        "    uint32_t d = m_dirty.fetch_add(1, std::memory_order_relaxed) + 1;\n"
        "    if (d < 32) return;\n"
        "    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(\n"
        "      dxvk::high_resolution_clock::now().time_since_epoch()).count();\n"
        "    int64_t prev = m_lastSaveMs.load(std::memory_order_relaxed);\n"
        "    if (nowMs - prev < 15000) return;\n"
        "    if (!m_lastSaveMs.compare_exchange_strong(prev, nowMs)) return;\n"
        "    m_dirty.store(0, std::memory_order_relaxed);\n"
        "    if (m_saving.exchange(true)) return;\n"
        "    Rc<DxvkPipelineCache> self(this);\n"
        "    std::thread([self] () {\n"
        "      self->saveToDisk();\n"
        "      self->m_saving.store(false);\n"
        "    }).detach();\n"
        "  }\n\n"
        "  void DxvkPipelineCache::saveToDisk() {\n"
        "    if (m_path.empty() || !m_handle || !m_anyCompiled.load(std::memory_order_relaxed)) return;\n"
        "    std::lock_guard<std::mutex> sl(m_saveMu);\n"
        "    size_t sz = 0;\n"
        "    if (m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &sz, nullptr) != VK_SUCCESS || sz == 0)\n"
        "      return;\n"
        "    std::vector<char> buf(sz);\n"
        "    if (m_vkd->vkGetPipelineCacheData(m_vkd->device(), m_handle, &sz, buf.data()) != VK_SUCCESS || sz == 0)\n"
        "      return;\n"
        "    std::string tmp = m_path + \".tmp\";\n"
        "    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);\n"
        "    if (out && out.write(buf.data(), std::streamsize(sz))) {\n"
        "      out.close();\n"
        "      std::rename(tmp.c_str(), m_path.c_str());\n"
        "    }\n"
        "  }")
replace(PCC,
        "  DxvkPipelineCache::~DxvkPipelineCache() {\n"
        "    m_vkd->vkDestroyPipelineCache(\n      m_vkd->device(), m_handle, nullptr);\n  }",
        "  DxvkPipelineCache::~DxvkPipelineCache() {\n"
        "    { std::lock_guard<std::mutex> l(g_hpPipeCacheMu);\n"
        "      if (g_hpPipeCache == this) g_hpPipeCache = nullptr; }\n"
        "    saveToDisk();\n"
        "    m_vkd->vkDestroyPipelineCache(\n      m_vkd->device(), m_handle, nullptr);\n  }")

# ---- Lock-free pipeline compile + Async graphics pipeline compilation (dxvk-async) --------------
# 1) Never hold sync::Spinlock m_mutex across vkCreateGraphicsPipelines / vkCreateComputePipelines
#    (previously a background state-cache compile stalled the render thread on a 100%-CPU spinlock).
# 2) Compile new graphics pipelines asynchronously on low-priority worker threads (default on
#    Android; DXVK_ASYNC=0 disables) so camera turns / new NPCs don't hitch for 30-150 ms.
GH = "src/dxvk/dxvk_graphics.h"
GC = "src/dxvk/dxvk_graphics.cpp"
CC = "src/dxvk/dxvk_compute.cpp"
PMH = "src/dxvk/dxvk_pipemanager.h"
PMC = "src/dxvk/dxvk_pipemanager.cpp"
SCC = "src/dxvk/dxvk_state_cache.cpp"

replace(GH,
        "    VkPipeline pipeline() const {\n      return m_pipeline;\n    }\n",
        "    VkPipeline pipeline() const {\n      return m_pipeline;\n    }\n\n"
        "    void setPipeline(VkPipeline pipe) {\n      m_pipeline = pipe;\n    }\n")
replace(GH,
        "    void compilePipeline(\n      const DxvkGraphicsPipelineStateInfo&    state,\n      const DxvkRenderPass*                   renderPass);\n",
        "    void compilePipeline(\n      const DxvkGraphicsPipelineStateInfo&    state,\n      const DxvkRenderPass*                   renderPass);\n\n"
        "    void compilePipelineAsync(\n      const DxvkGraphicsPipelineStateInfo&    state,\n      const DxvkRenderPass*                   renderPass);\n")

replace(PMH,
        "#include <mutex>\n#include <unordered_map>\n",
        "#include <atomic>\n#include <condition_variable>\n#include <mutex>\n#include <queue>\n#include <unordered_map>\n#include <vector>\n")
replace(PMH,
        "    void stopWorkerThreads() const;\n",
        "    void stopWorkerThreads();\n\n"
        "    bool asyncEnabled() const { return m_asyncEnabled; }\n\n"
        "    void queueAsyncCompile(\n"
        "            DxvkGraphicsPipeline*           pipe,\n"
        "      const DxvkGraphicsPipelineStateInfo&  state,\n"
        "      const DxvkRenderPass*                 renderPass);\n")
replace(PMH,
        "    std::unordered_map<\n"
        "      DxvkGraphicsPipelineShaders,\n"
        "      DxvkGraphicsPipeline,\n"
        "      DxvkHash, DxvkEq> m_graphicsPipelines;\n",
        "    std::unordered_map<\n"
        "      DxvkGraphicsPipelineShaders,\n"
        "      DxvkGraphicsPipeline,\n"
        "      DxvkHash, DxvkEq> m_graphicsPipelines;\n\n"
        "    struct AsyncJob {\n"
        "      DxvkGraphicsPipeline*         pipe;\n"
        "      DxvkGraphicsPipelineStateInfo state;\n"
        "      const DxvkRenderPass*         renderPass;\n"
        "    };\n"
        "    bool                            m_asyncEnabled = false;\n"
        "    std::atomic<bool>               m_asyncStop{false};\n"
        "    dxvk::mutex                     m_asyncMu;\n"
        "    dxvk::condition_variable        m_asyncCv;\n"
        "    std::queue<AsyncJob>            m_asyncQueue;\n"
        "    std::vector<dxvk::thread>       m_asyncThreads;\n"
        "    void asyncWorkerFunc();\n")

replace(PMC,
        '#include "dxvk_state_cache.h"\n',
        '#include "dxvk_state_cache.h"\n#include <sys/resource.h>\n')
replace(PMC,
        "    if (useStateCache != \"0\" && device->config().enableStateCache)\n      m_stateCache = new DxvkStateCache(device, this, passManager);\n  }",
        "    if (useStateCache != \"0\" && device->config().enableStateCache)\n      m_stateCache = new DxvkStateCache(device, this, passManager);\n\n"
        "    std::string asyncEnv = env::getEnvVar(\"DXVK_ASYNC\");\n"
        "#ifdef __ANDROID__\n"
        "    m_asyncEnabled = asyncEnv.empty() || asyncEnv != \"0\";\n"
        "#else\n"
        "    m_asyncEnabled = !asyncEnv.empty() && asyncEnv != \"0\";\n"
        "#endif\n"
        "    if (m_asyncEnabled) {\n"
        "      uint32_t n = 2;\n"
        "      Logger::warn(str::format(\"hp: DXVK async pipeline compiler enabled (\", n, \" threads)\"));\n"
        "      for (uint32_t i = 0; i < n; i++)\n"
        "        m_asyncThreads.emplace_back([this] () { asyncWorkerFunc(); });\n"
        "    }\n  }")
replace(PMC,
        "  DxvkPipelineManager::~DxvkPipelineManager() {\n    \n  }",
        "  DxvkPipelineManager::~DxvkPipelineManager() {\n    stopWorkerThreads();\n  }\n\n"
        "  void DxvkPipelineManager::queueAsyncCompile(\n"
        "          DxvkGraphicsPipeline*           pipe,\n"
        "    const DxvkGraphicsPipelineStateInfo&  state,\n"
        "    const DxvkRenderPass*                 renderPass) {\n"
        "    { std::lock_guard<dxvk::mutex> lock(m_asyncMu);\n"
        "      if (m_asyncStop.load()) return;\n"
        "      m_asyncQueue.push({ pipe, state, renderPass });\n"
        "    }\n"
        "    m_asyncCv.notify_one();\n"
        "  }\n\n"
        "  void DxvkPipelineManager::asyncWorkerFunc() {\n"
        "    env::setThreadName(\"dxvk-async\");\n"
        "    setpriority(PRIO_PROCESS, 0, 10);\n"
        "    for (;;) {\n"
        "      AsyncJob job;\n"
        "      { std::unique_lock<dxvk::mutex> lock(m_asyncMu);\n"
        "        m_asyncCv.wait(lock, [this] () { return !m_asyncQueue.empty() || m_asyncStop.load(); });\n"
        "        if (m_asyncQueue.empty()) break;\n"
        "        job = std::move(m_asyncQueue.front());\n"
        "        m_asyncQueue.pop();\n"
        "      }\n"
        "      job.pipe->compilePipelineAsync(job.state, job.renderPass);\n"
        "    }\n"
        "  }")
replace(PMC,
        "  void DxvkPipelineManager::stopWorkerThreads() const {\n"
        "    if (m_stateCache != nullptr)\n"
        "      m_stateCache->stopWorkerThreads();\n  }",
        "  void DxvkPipelineManager::stopWorkerThreads() {\n"
        "    if (m_stateCache != nullptr)\n"
        "      m_stateCache->stopWorkerThreads();\n"
        "    { std::lock_guard<dxvk::mutex> lock(m_asyncMu);\n"
        "      m_asyncStop.store(true);\n"
        "    }\n"
        "    m_asyncCv.notify_all();\n"
        "    for (auto& t : m_asyncThreads)\n"
        "      if (t.joinable()) t.join();\n"
        "    m_asyncThreads.clear();\n  }")

replace(GC,
        "  DxvkGraphicsPipeline::~DxvkGraphicsPipeline() {\n"
        "    for (const auto& instance : m_pipelines)\n"
        "      this->destroyPipeline(instance.pipeline());\n  }",
        "  DxvkGraphicsPipeline::~DxvkGraphicsPipeline() {\n"
        "    for (const auto& instance : m_pipelines)\n"
        "      if (instance.pipeline())\n"
        "        this->destroyPipeline(instance.pipeline());\n  }")
replace(GC,
        "  VkPipeline DxvkGraphicsPipeline::getPipelineHandle(\n"
        "    const DxvkGraphicsPipelineStateInfo& state,\n"
        "    const DxvkRenderPass*                renderPass) {\n"
        "    DxvkGraphicsPipelineInstance* instance = nullptr;\n\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "    \n"
        "      instance = this->findInstance(state, renderPass);\n"
        "      \n"
        "      if (instance)\n"
        "        return instance->pipeline();\n"
        "      \n"
        "      instance = this->createInstance(state, renderPass);\n"
        "    }\n"
        "    \n"
        "    if (!instance)\n"
        "      return VK_NULL_HANDLE;\n\n"
        "    this->writePipelineStateToCache(state, renderPass->format());\n"
        "    return instance->pipeline();\n"
        "  }\n\n\n"
        "  void DxvkGraphicsPipeline::compilePipeline(\n"
        "    const DxvkGraphicsPipelineStateInfo& state,\n"
        "    const DxvkRenderPass*                renderPass) {\n"
        "    std::lock_guard<sync::Spinlock> lock(m_mutex);\n\n"
        "    if (!this->findInstance(state, renderPass))\n"
        "      this->createInstance(state, renderPass);\n"
        "  }",
        "  VkPipeline DxvkGraphicsPipeline::getPipelineHandle(\n"
        "    const DxvkGraphicsPipelineStateInfo& state,\n"
        "    const DxvkRenderPass*                renderPass) {\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      DxvkGraphicsPipelineInstance* instance = this->findInstance(state, renderPass);\n"
        "      if (instance)\n"
        "        return instance->pipeline();\n"
        "      if (m_pipeMgr->asyncEnabled()\n"
        "       && !m_flags.test(DxvkGraphicsPipelineFlag::HasTransformFeedback)\n"
        "       && this->validatePipelineState(state)) {\n"
        "        m_pipelines.emplace_back(state, renderPass, VK_NULL_HANDLE);\n"
        "        m_pipeMgr->queueAsyncCompile(this, state, renderPass);\n"
        "        return VK_NULL_HANDLE;\n"
        "      }\n"
        "    }\n"
        "    if (!this->validatePipelineState(state))\n"
        "      return VK_NULL_HANDLE;\n"
        "    VkPipeline handle = this->createPipeline(state, renderPass);\n"
        "    if (!handle)\n"
        "      return VK_NULL_HANDLE;\n"
        "    bool inserted = false;\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      DxvkGraphicsPipelineInstance* instance = this->findInstance(state, renderPass);\n"
        "      if (instance) {\n"
        "        if (!instance->pipeline()) { instance->setPipeline(handle); inserted = true; }\n"
        "        else { this->destroyPipeline(handle); handle = instance->pipeline(); }\n"
        "      } else {\n"
        "        m_pipelines.emplace_back(state, renderPass, handle);\n"
        "        inserted = true;\n"
        "      }\n"
        "    }\n"
        "    if (inserted) {\n"
        "      m_pipeMgr->m_numGraphicsPipelines += 1;\n"
        "      this->writePipelineStateToCache(state, renderPass->format());\n"
        "      m_pipeMgr->m_cache->noteCompiled();\n"
        "    }\n"
        "    return handle;\n"
        "  }\n\n\n"
        "  void DxvkGraphicsPipeline::compilePipeline(\n"
        "    const DxvkGraphicsPipelineStateInfo& state,\n"
        "    const DxvkRenderPass*                renderPass) {\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      DxvkGraphicsPipelineInstance* instance = this->findInstance(state, renderPass);\n"
        "      if (instance && instance->pipeline())\n"
        "        return;\n"
        "    }\n"
        "    if (!this->validatePipelineState(state))\n"
        "      return;\n"
        "    VkPipeline handle = this->createPipeline(state, renderPass);\n"
        "    if (!handle)\n"
        "      return;\n"
        "    bool inserted = false;\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      DxvkGraphicsPipelineInstance* instance = this->findInstance(state, renderPass);\n"
        "      if (instance) {\n"
        "        if (!instance->pipeline()) { instance->setPipeline(handle); inserted = true; }\n"
        "        else { this->destroyPipeline(handle); }\n"
        "      } else {\n"
        "        m_pipelines.emplace_back(state, renderPass, handle);\n"
        "        inserted = true;\n"
        "      }\n"
        "    }\n"
        "    if (inserted) {\n"
        "      m_pipeMgr->m_numGraphicsPipelines += 1;\n"
        "      m_pipeMgr->m_cache->noteCompiled();\n"
        "    }\n"
        "  }\n\n\n"
        "  void DxvkGraphicsPipeline::compilePipelineAsync(\n"
        "    const DxvkGraphicsPipelineStateInfo& state,\n"
        "    const DxvkRenderPass*                renderPass) {\n"
        "    this->compilePipeline(state, renderPass);\n"
        "    this->writePipelineStateToCache(state, renderPass->format());\n"
        "  }")

# Also release spinlock during compute pipeline creation & mark cache dirty
replace(CC,
        "  VkPipeline DxvkComputePipeline::getPipelineHandle(\n"
        "    const DxvkComputePipelineStateInfo& state) {\n"
        "    DxvkComputePipelineInstance* instance = nullptr;\n\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n\n"
        "      instance = this->findInstance(state);\n\n"
        "      if (instance)\n"
        "        return instance->pipeline();\n"
        "    \n"
        "      // If no pipeline instance exists with the given state\n"
        "      // vector, create a new one and add it to the list.\n"
        "      instance = this->createInstance(state);\n"
        "    }\n"
        "    \n"
        "    if (!instance)\n"
        "      return VK_NULL_HANDLE;\n\n"
        "    this->writePipelineStateToCache(state);\n"
        "    return instance->pipeline();\n"
        "  }\n\n\n"
        "  void DxvkComputePipeline::compilePipeline(\n"
        "    const DxvkComputePipelineStateInfo& state) {\n"
        "    std::lock_guard<sync::Spinlock> lock(m_mutex);\n\n"
        "    if (!this->findInstance(state))\n"
        "      this->createInstance(state);\n"
        "  }",
        "  VkPipeline DxvkComputePipeline::getPipelineHandle(\n"
        "    const DxvkComputePipelineStateInfo& state) {\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      DxvkComputePipelineInstance* instance = this->findInstance(state);\n"
        "      if (instance)\n"
        "        return instance->pipeline();\n"
        "    }\n"
        "    VkPipeline handle = this->createPipeline(state);\n"
        "    if (!handle)\n"
        "      return VK_NULL_HANDLE;\n"
        "    bool inserted = false;\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      DxvkComputePipelineInstance* instance = this->findInstance(state);\n"
        "      if (instance) { this->destroyPipeline(handle); handle = instance->pipeline(); }\n"
        "      else { m_pipelines.emplace_back(state, handle); inserted = true; }\n"
        "    }\n"
        "    if (inserted) {\n"
        "      m_pipeMgr->m_numComputePipelines += 1;\n"
        "      this->writePipelineStateToCache(state);\n"
        "      m_pipeMgr->m_cache->noteCompiled();\n"
        "    }\n"
        "    return handle;\n"
        "  }\n\n\n"
        "  void DxvkComputePipeline::compilePipeline(\n"
        "    const DxvkComputePipelineStateInfo& state) {\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      if (this->findInstance(state)) return;\n"
        "    }\n"
        "    VkPipeline handle = this->createPipeline(state);\n"
        "    if (!handle) return;\n"
        "    bool inserted = false;\n"
        "    { std::lock_guard<sync::Spinlock> lock(m_mutex);\n"
        "      if (this->findInstance(state)) this->destroyPipeline(handle);\n"
        "      else { m_pipelines.emplace_back(state, handle); inserted = true; }\n"
        "    }\n"
        "    if (inserted) {\n"
        "      m_pipeMgr->m_numComputePipelines += 1;\n"
        "      m_pipeMgr->m_cache->noteCompiled();\n"
        "    }\n"
        "  }")

# Lower state-cache compiler thread count (2 on Android) & priority (nice +10) so pre-warming
# does not starve Unity's main thread or job workers.
replace(SCC,
        '#include "dxvk_state_cache.h"\n',
        '#include "dxvk_state_cache.h"\n#include <sys/resource.h>\n')
replace(SCC,
        "    if (numWorkers <  1) numWorkers =  1;\n    if (numWorkers > 32) numWorkers = 32;\n",
        "    if (numWorkers <  1) numWorkers =  1;\n"
        "#ifdef __ANDROID__\n"
        "    if (numWorkers >  2) numWorkers =  2;\n"
        "#else\n"
        "    if (numWorkers > 32) numWorkers = 32;\n"
        "#endif\n")
replace(SCC,
        '    env::setThreadName("dxvk-shader");\n',
        '    env::setThreadName("dxvk-shader");\n    setpriority(PRIO_PROCESS, 0, 10);\n')

# Android Bionic default std::thread stack is 1 MB, which overflows during Adreno SPIR-V/shader
# compilation on complex Unity shaders. Give every dxvk::thread an 8 MB stack + 64 KB sigaltstack.
replace("src/util/thread.h", "#pragma once\n",
        "#ifdef __ANDROID__\n#include <pthread.h>\n#include <signal.h>\n#include <stdlib.h>\n#endif\n#pragma once\n")
replace("src/util/thread.h", "  using thread             = std::thread;\n",
        "#ifdef __ANDROID__\n"
        "  /* ANDROID_8MB_THREAD: Bionic default std::thread stack is 1 MB; Adreno shader compiler needs more. */\n"
        "  class thread {\n"
        "    struct State { std::function<void()> fn; };\n"
        "    static void* entry(void* arg) {\n"
        "      stack_t ss; ss.ss_sp = malloc(65536); ss.ss_size = 65536; ss.ss_flags = 0;\n"
        "      if (ss.ss_sp) sigaltstack(&ss, nullptr);\n"
        "      auto* st = static_cast<State*>(arg);\n"
        "      st->fn();\n"
        "      delete st;\n"
        "      return nullptr;\n"
        "    }\n"
        "    pthread_t m_th{};\n"
        "    bool m_joinable = false;\n"
        "  public:\n"
        "    thread() = default;\n"
        "    explicit thread(std::function<void()>&& fn) {\n"
        "      pthread_attr_t attr;\n"
        "      pthread_attr_init(&attr);\n"
        "      pthread_attr_setstacksize(&attr, 8u << 20);\n"
        "      auto* st = new State{std::move(fn)};\n"
        "      if (pthread_create(&m_th, &attr, entry, st) != 0) {\n"
        "        delete st;\n"
        "        pthread_attr_destroy(&attr);\n"
        "        throw DxvkError(\"Failed to create thread\");\n"
        "      }\n"
        "      pthread_attr_destroy(&attr);\n"
        "      m_joinable = true;\n"
        "    }\n"
        "    thread(const thread&) = delete;\n"
        "    thread& operator=(const thread&) = delete;\n"
        "    thread(thread&& o) noexcept : m_th(o.m_th), m_joinable(o.m_joinable) { o.m_joinable = false; }\n"
        "    thread& operator=(thread&& o) noexcept {\n"
        "      if (m_joinable) std::terminate();\n"
        "      m_th = o.m_th; m_joinable = o.m_joinable; o.m_joinable = false;\n"
        "      return *this;\n"
        "    }\n"
        "    ~thread() { if (m_joinable) std::terminate(); }\n"
        "    bool joinable() const { return m_joinable; }\n"
        "    void join() { if (m_joinable) { pthread_join(m_th, nullptr); m_joinable = false; } }\n"
        "    void detach() { if (m_joinable) { pthread_detach(m_th); m_joinable = false; } }\n"
        "    static uint32_t hardware_concurrency() { return std::thread::hardware_concurrency(); }\n"
        "  };\n"
        "#else\n"
        "  using thread             = std::thread;\n"
        "#endif\n")

(args.output / "version.h").write_text('#define DXVK_VERSION "native-1.9.2b-hp-android"\n')
(args.output / "PINNED_REVISION").write_text(PIN + "\n")
(args.output / "PREPARE_SHA256").write_text(hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest() + "\n")
print("Prepared DXVK", PIN, "->", args.output)
