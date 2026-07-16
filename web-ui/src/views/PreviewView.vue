<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'

const videoEl = ref<HTMLVideoElement | null>(null)
const status = ref('连接中…')
const error = ref('')

let ws: WebSocket | null = null
let ms: MediaSource | null = null
let sb: SourceBuffer | null = null
let queue: ArrayBuffer[] = []
let codec = ''
let closed = false
let reconnectTimer: number | undefined
let objectUrl = ''

function mimeFor(c: string) {
  return `video/mp4; codecs="${c}"`
}

function flush() {
  if (!sb || sb.updating || queue.length === 0) return
  const buf = queue.shift()!
  try {
    sb.appendBuffer(buf)
  } catch (e) {
    error.value = `append failed: ${e}`
  }
}

function resetPlayer() {
  queue = []
  sb = null
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

function startMse(c: string) {
  resetPlayer()
  codec = c
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
  v.play().catch(() => {
    /* autoplay may need mute; already muted */
  })
  ms.addEventListener('sourceopen', () => {
    if (!ms) return
    sb = ms.addSourceBuffer(mime)
    sb.mode = 'sequence'
    sb.addEventListener('updateend', flush)
    flush()
    status.value = '播放中'
  })
}

function scheduleReconnect() {
  if (closed) return
  status.value = '重连中…'
  reconnectTimer = window.setTimeout(connect, 1000)
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
      <span class="muted">{{ status }}</span>
    </header>
    <p v-if="error" class="error">{{ error }}</p>
    <div class="stage">
      <video ref="videoEl" muted autoplay playsinline />
    </div>
    <p class="muted tip">子码流 H.264 → fMP4 / WebSocket / MSE。断线自动重连。</p>
  </div>
</template>

<style scoped>
.preview { display: grid; gap: 0.75rem; height: 100%; }
header { display: flex; align-items: baseline; gap: 1rem; }
h2 { margin: 0; font-size: 1.25rem; }
.stage {
  background: #000;
  border: 1px solid var(--border);
  border-radius: 8px;
  min-height: 320px;
  display: flex;
  align-items: center;
  justify-content: center;
  overflow: hidden;
}
video { width: 100%; max-height: calc(100vh - 180px); background: #000; }
.tip { font-size: 0.85rem; }
</style>
