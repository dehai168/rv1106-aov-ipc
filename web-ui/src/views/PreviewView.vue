<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'

const videoEl = ref<HTMLVideoElement | null>(null)
const status = ref('连接中…')
const error = ref('')
const debug = ref('')

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

function mimeFor(c: string) {
  return `video/mp4; codecs="${c}"`
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
  debug.value = `q=${queue.length} app=${appended} buf=${br} t=${v ? v.currentTime.toFixed(2) : '-'} ${wh} ${v?.paused ? 'paused' : 'playing'}`
}

/** Live MSE: keep currentTime inside the newest buffered range. */
function chaseLive() {
  const v = videoEl.value
  if (!v || v.buffered.length === 0) return
  const start = v.buffered.start(0)
  const end = v.buffered.end(v.buffered.length - 1)
  const target = Math.max(start, end - 0.35)
  if (v.currentTime < start - 0.05 || v.currentTime > end || end - v.currentTime > 1.5) {
    try {
      v.currentTime = target
    } catch {
      /* ignore seek abort */
    }
  }
  if (v.paused && end - start > 0.2) {
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
    // sequence: ignore tfdt, assign contiguous timestamps (better for live IPC).
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
    chaseTimer = window.setInterval(chaseLive, 500)
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
  const url = `${proto}//${location.host}/api/v1/preview/ws`
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
  <div class="preview">
    <header>
      <h2>实时预览</h2>
      <strong class="status">{{ status }}</strong>
    </header>
    <p v-if="error" class="error">{{ error }}</p>
    <p class="muted tip">{{ debug || '等待调试信息…' }}</p>
    <div class="stage">
      <video ref="videoEl" muted autoplay playsinline controls />
    </div>
    <p class="muted tip">子码流 H.264 → fMP4 / WebSocket / MSE。断线自动重连。</p>
  </div>
</template>

<style scoped>
.preview { display: grid; gap: 0.75rem; height: 100%; }
header { display: flex; align-items: baseline; gap: 1rem; }
h2 { margin: 0; font-size: 1.25rem; }
.status { font-size: 0.95rem; color: var(--accent, #3d8bfd); }
.stage {
  background: #111;
  border: 1px solid var(--border);
  border-radius: 8px;
  min-height: 320px;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
}
video { width: 100%; max-height: calc(100vh - 180px); background: #222; min-height: 240px; }
.tip { font-size: 0.85rem; }
</style>
