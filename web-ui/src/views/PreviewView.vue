<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { alarmSnapshotUrl, previewSnapshot } from '../api'

type AspectMode = 'fit' | 'fill' | '16:9' | '4:3'
type StreamSel = 'sub' | 'main'

const videoEl = ref<HTMLVideoElement | null>(null)
const stageEl = ref<HTMLElement | null>(null)
const zoomWrapEl = ref<HTMLElement | null>(null)
const status = ref('连接中…')
const error = ref('')
const debug = ref('')
const aspect = ref<AspectMode>('fit')
const stream = ref<StreamSel>('sub')
const snapping = ref(false)
const zoom = ref(1)
const panX = ref(0)
const panY = ref(0)

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

const playing = computed(
  () => !error.value && (status.value.includes('播放') || status.value.includes('推流')),
)

const videoTransform = computed(
  () => `translate(${panX.value}px, ${panY.value}px) scale(${zoom.value})`,
)

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

function bumpDebug() {
  const v = videoEl.value
  let br = 'empty'
  if (v && v.buffered.length > 0) {
    br = `${v.buffered.start(0).toFixed(2)}-${v.buffered.end(v.buffered.length - 1).toFixed(2)}`
  }
  const wh = v ? `${v.videoWidth}x${v.videoHeight}` : '-'
  debug.value = `${stream.value} q=${queue.length} app=${appended} buf=${br} t=${v ? v.currentTime.toFixed(2) : '-'} ${wh} ${v?.paused ? 'paused' : 'playing'}`
}

/** Live MSE: keep currentTime inside the newest buffered range. */
function chaseLive() {
  const v = videoEl.value
  if (!v || v.buffered.length === 0) return
  const start = v.buffered.start(0)
  const end = v.buffered.end(v.buffered.length - 1)
  /* Stay close to live edge to avoid multi-second backlog stutter. */
  const target = Math.max(start, end - 0.2)
  if (v.currentTime < start - 0.05 || v.currentTime > end || end - v.currentTime > 0.8) {
    try {
      v.currentTime = target
    } catch {
      /* ignore seek abort */
    }
  }
  if (v.paused && end - start > 0.15) {
    v.play().catch(() => {
      /* ignore */
    })
  }
  bumpDebug()
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
        const j = JSON.parse(ev.data) as { codec?: string; err?: string; stream?: string }
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
    /* Drop backlog when browser decode/network is slow — prefer live over smooth catch-up. */
    if (queue.length > 24) {
      queue = queue.slice(-8)
    }
    queue.push(ab)
    flush()
    bumpDebug()
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
  if (ws) {
    ws.onclose = null
    ws.close()
    ws = null
  }
  resetPlayer()
  resetZoom()
  connect()
}

function onStreamChange() {
  reconnectNow()
}

async function toggleFullscreen() {
  const el = stageEl.value
  if (!el) return
  try {
    if (document.fullscreenElement) {
      await document.exitFullscreen()
    } else {
      await el.requestFullscreen()
    }
  } catch (e) {
    ElMessage.error(`全屏失败: ${e}`)
  }
}

async function onSnapshot() {
  if (snapping.value) return
  snapping.value = true
  try {
    const r = await previewSnapshot()
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

onMounted(() => {
  closed = false
  connect()
})

onUnmounted(() => {
  closed = true
  if (reconnectTimer) window.clearTimeout(reconnectTimer)
  if (ws) {
    ws.onclose = null
    ws.close()
    ws = null
  }
  resetPlayer()
})
</script>

<template>
  <section class="page preview-page">
    <div class="head">
      <div>
        <h2 class="page-title">实时预览</h2>
        <p class="page-desc">H.264 → fMP4 / WebSocket / MSE；主码流为辅助 H.264 预览通道（非录像 H.265）。</p>
      </div>
      <el-tag :type="error ? 'danger' : playing ? 'success' : 'info'">
        {{ status }}
      </el-tag>
    </div>

    <div class="toolbar">
      <el-radio-group v-model="stream" size="small" @change="onStreamChange">
        <el-radio-button label="sub">子码流</el-radio-button>
        <el-radio-button label="main">主预览</el-radio-button>
      </el-radio-group>
      <el-button size="small" @click="reconnectNow">重连</el-button>
      <el-button size="small" :loading="snapping" type="primary" @click="onSnapshot">抓图</el-button>
      <el-button size="small" @click="toggleFullscreen">全屏</el-button>
      <el-radio-group v-model="aspect" size="small">
        <el-radio-button label="fit">适应</el-radio-button>
        <el-radio-button label="fill">填充</el-radio-button>
        <el-radio-button label="16:9">16:9</el-radio-button>
        <el-radio-button label="4:3">4:3</el-radio-button>
      </el-radio-group>
      <div class="zoom-bar">
        <span class="zoom-label">缩放</span>
        <el-slider v-model="zoom" :min="1" :max="4" :step="0.1" style="width: 120px" />
        <el-button size="small" text @click="resetZoom">复位</el-button>
      </div>
    </div>

    <el-alert v-if="error" :title="error" type="error" show-icon :closable="false" />
    <div ref="stageEl" class="preview-stage" :data-aspect="aspect">
      <div
        ref="zoomWrapEl"
        class="zoom-wrap"
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
    </div>
    <el-text v-if="debug" type="info" size="small">{{ debug }}</el-text>
  </section>
</template>

<style scoped>
.preview-page { max-width: none; }
.head {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  gap: 1rem;
  flex-wrap: wrap;
}
.toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.6rem;
}
.zoom-bar {
  display: flex;
  align-items: center;
  gap: 0.4rem;
  margin-left: 0.4rem;
}
.zoom-label {
  font-size: 12px;
  color: var(--ipc-muted);
}
.preview-stage {
  min-height: 360px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: #0b0f14;
  overflow: hidden;
}
.preview-stage:fullscreen {
  width: 100%;
  height: 100%;
  min-height: 100%;
  background: #000;
}
.zoom-wrap {
  width: 100%;
  height: 100%;
  min-height: 280px;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
  touch-action: none;
  cursor: grab;
}
.zoom-wrap:active { cursor: grabbing; }
.preview-video {
  width: 100%;
  max-height: calc(100vh - 220px);
  background: #0b0f14;
  min-height: 280px;
  display: block;
  object-fit: contain;
  transform-origin: center center;
  transition: transform 0.05s linear;
}
.preview-stage:fullscreen .preview-video {
  max-height: 100vh;
  height: 100%;
}
.preview-video.aspect-fill {
  object-fit: cover;
}
.preview-video.aspect-16-9 {
  aspect-ratio: 16 / 9;
  height: auto;
  object-fit: contain;
}
.preview-video.aspect-4-3 {
  aspect-ratio: 4 / 3;
  height: auto;
  object-fit: contain;
}
</style>
