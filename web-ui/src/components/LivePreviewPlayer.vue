<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'
import {
  FullScreen,
  Camera,
  Switch,
  Crop,
  ZoomIn,
  ZoomOut,
  RefreshRight,
} from '@element-plus/icons-vue'
import { alarmSnapshotUrl, getVideoEncode, previewSnapshot, type StreamParams } from '../api'

type AspectMode = 'fit' | 'fill' | '16:9' | '4:3'
type StreamSel = 'sub' | 'main'

const props = withDefaults(
  defineProps<{
    /** When false, hide right toolbar (still plays). */
    controls?: boolean
    /** Prefer starting stream. */
    initialStream?: StreamSel
    /** Compact embed for side panels. */
    compact?: boolean
  }>(),
  {
    controls: true,
    initialStream: 'sub',
    compact: false,
  },
)

const videoEl = ref<HTMLVideoElement | null>(null)
const stageEl = ref<HTMLElement | null>(null)
const status = ref('连接中…')
const error = ref('')
const aspect = ref<AspectMode>('fit')
const stream = ref<StreamSel>(props.initialStream)
const snapping = ref(false)
const zoom = ref(1)
const panX = ref(0)
const panY = ref(0)
const encodeSub = ref<StreamParams | null>(null)
const encodeMain = ref<StreamParams | null>(null)
const videoW = ref(0)
const videoH = ref(0)

let ws: WebSocket | null = null
let ms: MediaSource | null = null
let sb: SourceBuffer | null = null
let queue: ArrayBuffer[] = []
let closed = false
let reconnectTimer: number | undefined
let objectUrl = ''
let waitingInit = true
let appended = 0
let chaseTimer: number | undefined
let dragging = false
let dragLastX = 0
let dragLastY = 0

const videoTransform = computed(
  () => `translate(${panX.value}px, ${panY.value}px) scale(${zoom.value})`,
)

const hud = computed(() => {
  const enc = stream.value === 'main' ? encodeMain.value : encodeSub.value
  const w = videoW.value || enc?.w || 0
  const h = videoH.value || enc?.h || 0
  const fps = enc?.fps ?? 0
  const br = enc?.bitrate_kbps ?? 0
  return {
    res: w && h ? `${w}×${h}` : '—',
    bitrate: br ? `${br} kbps` : '—',
    fps: fps ? `${fps} fps` : '—',
  }
})

const cursorClass = computed(() => (zoom.value > 1 ? 'zoomed' : 'default'))

function mimeFor(c: string) {
  return `video/mp4; codecs="${c}"`
}

function resetZoom() {
  zoom.value = 1
  panX.value = 0
  panY.value = 0
}

function flush() {
  if (!sb || sb.updating || queue.length === 0) return
  const buf = queue.shift()!
  try {
    sb.appendBuffer(buf)
    appended++
  } catch (e) {
    error.value = `append failed: ${e}`
    queue.unshift(buf)
  }
}

function resetPlayer() {
  queue = []
  sb = null
  waitingInit = true
  appended = 0
  if (chaseTimer) {
    window.clearInterval(chaseTimer)
    chaseTimer = undefined
  }
  if (ms) {
    try {
      if (ms.readyState === 'open') ms.endOfStream()
    } catch {
      /* ignore */
    }
    ms = null
  }
  if (videoEl.value) {
    videoEl.value.removeAttribute('src')
    videoEl.value.load()
  }
  if (objectUrl) {
    URL.revokeObjectURL(objectUrl)
    objectUrl = ''
  }
}

function chaseLive() {
  const v = videoEl.value
  if (!v || v.buffered.length === 0) return
  const start = v.buffered.start(0)
  const end = v.buffered.end(v.buffered.length - 1)
  const target = Math.max(start, end - 0.2)
  if (v.currentTime < start - 0.05 || v.currentTime > end || end - v.currentTime > 0.8) {
    try {
      v.currentTime = target
    } catch {
      /* ignore */
    }
  }
  if (v.paused && end - start > 0.15) {
    v.play().catch(() => {})
  }
  if (v.videoWidth) videoW.value = v.videoWidth
  if (v.videoHeight) videoH.value = v.videoHeight
  evictSourceBuffer(end)
}

/** Keep MSE buffer short to avoid QuotaExceeded / long-run black screen. */
function evictSourceBuffer(liveEnd: number) {
  if (!sb || sb.updating || !ms || ms.readyState !== 'open') return
  try {
    if (sb.buffered.length === 0) return
    const b0 = sb.buffered.start(0)
    const keepFrom = Math.max(0, liveEnd - 4)
    if (b0 < keepFrom - 0.5) {
      sb.remove(b0, keepFrom)
    }
  } catch {
    /* ignore */
  }
}

function startMse(c: string) {
  resetPlayer()
  const mime = mimeFor(c)
  if (!window.MediaSource || !MediaSource.isTypeSupported(mime)) {
    error.value = `浏览器不支持 ${mime}`
    status.value = '失败'
    return
  }
  ms = new MediaSource()
  objectUrl = URL.createObjectURL(ms)
  const v = videoEl.value!
  v.src = objectUrl
  ms.addEventListener('sourceopen', () => {
    if (!ms) return
    try {
      ms.duration = Number.POSITIVE_INFINITY
    } catch {
      try {
        ms.duration = 1e9
      } catch {
        /* ignore */
      }
    }
    sb = ms.addSourceBuffer(mime)
    sb.mode = 'sequence'
    sb.addEventListener('updateend', () => {
      flush()
      chaseLive()
      if (appended >= 2) status.value = '播放中'
    })
    sb.addEventListener('error', () => {
      error.value = 'SourceBuffer error'
    })
    flush()
    status.value = '缓冲中…'
    chaseTimer = window.setInterval(chaseLive, 250)
  })
}

function scheduleReconnect() {
  if (closed) return
  status.value = '重连中…'
  reconnectTimer = window.setTimeout(connect, 2000)
}

function connect() {
  if (closed) return
  error.value = ''
  status.value = '连接中…'
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  const url = `${proto}//${location.host}/api/v1/preview/ws?stream=${stream.value}`
  ws = new WebSocket(url)
  ws.binaryType = 'arraybuffer'
  ws.onopen = () => {
    status.value = '等待码流…'
  }
  ws.onmessage = (ev) => {
    if (typeof ev.data === 'string') {
      try {
        const j = JSON.parse(ev.data) as { codec?: string; err?: string }
        if (j.err) {
          error.value = j.err
          return
        }
        if (j.codec) startMse(j.codec)
      } catch (e) {
        error.value = String(e)
      }
      return
    }
    const ab = ev.data as ArrayBuffer
    const u8 = new Uint8Array(ab)
    const isFtyp =
      ab.byteLength >= 8 &&
      u8[4] === 0x66 &&
      u8[5] === 0x74 &&
      u8[6] === 0x79 &&
      u8[7] === 0x70
    if (waitingInit) {
      if (!isFtyp) return
      waitingInit = false
    }
    if (queue.length > 24) queue = queue.slice(-8)
    queue.push(ab)
    flush()
  }
  ws.onerror = () => {
    error.value = 'WebSocket 错误'
  }
  ws.onclose = () => {
    ws = null
    resetPlayer()
    scheduleReconnect()
  }
}

function reconnectNow() {
  if (reconnectTimer) {
    window.clearTimeout(reconnectTimer)
    reconnectTimer = undefined
  }
  stopSocket()
  resetPlayer()
  resetZoom()
  connect()
}

/**
 * Ask the server to stop pushing frames, then close. Sending data on a socket
 * that is already closing makes Chrome log
 * "Data frame received after close" during main/sub switches.
 */
function stopSocket() {
  const sock = ws
  ws = null
  if (!sock) return
  sock.onmessage = null
  sock.onerror = null
  sock.onclose = null
  try {
    if (sock.readyState === WebSocket.OPEN) {
      sock.send('stop')
      window.setTimeout(() => {
        try {
          sock.close()
        } catch {
          /* ignore */
        }
      }, 120)
      return
    }
    sock.close()
  } catch {
    /* ignore */
  }
}

function toggleStream() {
  stream.value = stream.value === 'sub' ? 'main' : 'sub'
}

function cycleAspect() {
  const order: AspectMode[] = ['fit', 'fill', '16:9', '4:3']
  const i = order.indexOf(aspect.value)
  aspect.value = order[(i + 1) % order.length]
}

function zoomIn() {
  zoom.value = Math.min(4, Math.round((zoom.value + 0.25) * 100) / 100)
}

function zoomOut() {
  const next = Math.max(1, Math.round((zoom.value - 0.25) * 100) / 100)
  zoom.value = next
  if (next <= 1) {
    panX.value = 0
    panY.value = 0
  }
}

async function toggleFullscreen() {
  const el = stageEl.value
  if (!el) return
  try {
    if (document.fullscreenElement) await document.exitFullscreen()
    else await el.requestFullscreen()
  } catch (e) {
    ElMessage.error(`全屏失败: ${e}`)
  }
}

async function onSnapshot() {
  if (snapping.value) return
  snapping.value = true
  try {
    /* Snapshot must match the stream currently on screen (main => main size). */
    const r = await previewSnapshot(stream.value)
    if (r.code !== 0 || !r.data?.file) {
      ElMessage.error(r.msg || '抓图失败')
      return
    }
    const href = alarmSnapshotUrl(r.data.file)
    const a = document.createElement('a')
    a.href = href
    a.download = r.data.file
    a.rel = 'noopener'
    document.body.appendChild(a)
    a.click()
    a.remove()
    ElMessage.success(`已抓图 ${r.data.file}`)
  } catch (e) {
    ElMessage.error(`抓图失败: ${e}`)
  } finally {
    snapping.value = false
  }
}

function onWheel(ev: WheelEvent) {
  ev.preventDefault()
  const next = Math.min(4, Math.max(1, zoom.value + (ev.deltaY < 0 ? 0.15 : -0.15)))
  zoom.value = Math.round(next * 100) / 100
  if (zoom.value <= 1) {
    panX.value = 0
    panY.value = 0
  }
}

function onPointerDown(ev: PointerEvent) {
  if (zoom.value <= 1) return
  dragging = true
  dragLastX = ev.clientX
  dragLastY = ev.clientY
  ;(ev.target as HTMLElement).setPointerCapture?.(ev.pointerId)
}

function onPointerMove(ev: PointerEvent) {
  if (!dragging || zoom.value <= 1) return
  panX.value += ev.clientX - dragLastX
  panY.value += ev.clientY - dragLastY
  dragLastX = ev.clientX
  dragLastY = ev.clientY
}

function onPointerUp() {
  dragging = false
}

async function loadEncodeMeta() {
  try {
    const r = await getVideoEncode()
    if (r.code === 0) {
      encodeMain.value = r.data.main
      encodeSub.value = r.data.sub
    }
  } catch {
    /* ignore */
  }
}

watch(stream, () => reconnectNow())

onMounted(() => {
  closed = false
  loadEncodeMeta()
  connect()
})

onUnmounted(() => {
  closed = true
  if (reconnectTimer) window.clearTimeout(reconnectTimer)
  if (chaseTimer) window.clearInterval(chaseTimer)
  stopSocket()
  resetPlayer()
})

defineExpose({ reconnectNow, stream, status })
</script>

<template>
  <div
    ref="stageEl"
    class="live-preview"
    :class="{ compact, 'has-controls': controls }"
  >
    <div
      class="zoom-wrap"
      :class="cursorClass"
      @wheel.prevent="onWheel"
      @pointerdown="onPointerDown"
      @pointermove="onPointerMove"
      @pointerup="onPointerUp"
      @pointercancel="onPointerUp"
    >
      <video
        ref="videoEl"
        class="preview-video"
        :class="`aspect-${aspect === '16:9' ? '16-9' : aspect === '4:3' ? '4-3' : aspect}`"
        :style="{ transform: videoTransform }"
        muted
        autoplay
        playsinline
      />
    </div>

    <div class="hud-meta">
      <span>{{ hud.res }}</span>
      <span>{{ hud.bitrate }}</span>
      <span>{{ hud.fps }}</span>
    </div>

    <div v-if="controls" class="hud-tools">
      <el-tooltip :content="`码流：${stream === 'sub' ? '子码流' : '主预览'}（点击切换）`" placement="left">
        <el-button circle size="small" @click="toggleStream">
          <el-icon><Switch /></el-icon>
        </el-button>
      </el-tooltip>
      <el-tooltip content="抓图" placement="left">
        <el-button circle size="small" :loading="snapping" @click="onSnapshot">
          <el-icon><Camera /></el-icon>
        </el-button>
      </el-tooltip>
      <el-tooltip content="全屏" placement="left">
        <el-button circle size="small" @click="toggleFullscreen">
          <el-icon><FullScreen /></el-icon>
        </el-button>
      </el-tooltip>
      <el-tooltip :content="`比例：${aspect}`" placement="left">
        <el-button circle size="small" @click="cycleAspect">
          <el-icon><Crop /></el-icon>
        </el-button>
      </el-tooltip>
      <el-tooltip content="放大" placement="left">
        <el-button circle size="small" @click="zoomIn">
          <el-icon><ZoomIn /></el-icon>
        </el-button>
      </el-tooltip>
      <el-tooltip content="缩小" placement="left">
        <el-button circle size="small" @click="zoomOut">
          <el-icon><ZoomOut /></el-icon>
        </el-button>
      </el-tooltip>
      <el-tooltip content="复位缩放" placement="left">
        <el-button circle size="small" @click="resetZoom">
          <el-icon><RefreshRight /></el-icon>
        </el-button>
      </el-tooltip>
    </div>

    <div class="hud-status" :class="{ err: !!error }">
      {{ error || status }}
    </div>
  </div>
</template>

<style scoped>
.live-preview {
  position: relative;
  width: 100%;
  height: 100%;
  min-height: 240px;
  background: #0b0f14;
  overflow: hidden;
}
.live-preview:fullscreen {
  width: 100%;
  height: 100%;
  background: #000;
}
.zoom-wrap {
  width: 100%;
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  touch-action: none;
}
.zoom-wrap.default {
  cursor: default;
}
.zoom-wrap.zoomed {
  cursor: move;
}
.preview-video {
  width: 100%;
  height: 100%;
  display: block;
  object-fit: contain;
  background: #0b0f14;
  transform-origin: center center;
  transition: transform 0.05s linear;
}
.preview-video.aspect-fill {
  object-fit: cover;
}
.preview-video.aspect-16-9 {
  aspect-ratio: 16 / 9;
  height: auto;
  max-height: 100%;
  object-fit: contain;
}
.preview-video.aspect-4-3 {
  aspect-ratio: 4 / 3;
  height: auto;
  max-height: 100%;
  object-fit: contain;
}
.hud-meta {
  position: absolute;
  left: 12px;
  bottom: 12px;
  display: flex;
  gap: 0.65rem;
  padding: 0.35rem 0.55rem;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.55);
  color: #e8eef4;
  font-size: 12px;
  letter-spacing: 0.02em;
  pointer-events: none;
  z-index: 2;
}
.hud-tools {
  position: absolute;
  right: 12px;
  top: 50%;
  transform: translateY(-50%);
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 0.45rem;
  z-index: 2;
}
.hud-tools :deep(.el-button) {
  width: 34px;
  height: 34px;
  margin: 0;
  padding: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.55);
  border-color: rgba(255, 255, 255, 0.18);
  color: #fff;
}
.hud-tools :deep(.el-button + .el-button) {
  margin-left: 0;
}
.hud-tools :deep(.el-button:hover) {
  background: rgba(43, 155, 187, 0.85);
  border-color: transparent;
  color: #fff;
}
.hud-status {
  position: absolute;
  left: 12px;
  top: 12px;
  padding: 0.2rem 0.45rem;
  border-radius: 4px;
  background: rgba(0, 0, 0, 0.45);
  color: #b8c4d0;
  font-size: 11px;
  pointer-events: none;
  z-index: 2;
}
.hud-status.err {
  color: #ffb4b4;
}
.compact {
  min-height: 280px;
  border-radius: 6px;
}
</style>
