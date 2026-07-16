<script setup lang="ts">
import { onMounted, ref } from 'vue'
import {
  getVideoEncode,
  getVideoImage,
  setVideoEncode,
  setVideoImage,
  type VideoEncodeConfig,
  type VideoImageConfig,
} from '../api'

const loading = ref(true)
const savingImage = ref(false)
const savingEncode = ref(false)
const error = ref('')
const ok = ref('')
const image = ref<VideoImageConfig>({
  brightness: 50,
  contrast: 50,
  saturation: 50,
  mirror: false,
  flip: false,
})
const encode = ref<VideoEncodeConfig>({
  main: { w: 1920, h: 1080, fps: 15, bitrate_kbps: 2048, gop: 30 },
  sub: { w: 704, h: 576, fps: 15, bitrate_kbps: 1024, gop: 30 },
  stream_up: false,
})

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    const [img, enc] = await Promise.all([getVideoImage(), getVideoEncode()])
    if (img.code !== 0) {
      error.value = img.msg || '读取图像参数失败'
      return
    }
    if (enc.code !== 0) {
      error.value = enc.msg || '读取编码参数失败'
      return
    }
    image.value = img.data
    encode.value = enc.data
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}

async function onSaveImage() {
  error.value = ''
  ok.value = ''
  savingImage.value = true
  try {
    const r = await setVideoImage({ ...image.value, apply: true })
    if (r.code !== 0) {
      error.value = r.msg || '保存图像参数失败'
      return
    }
    image.value = r.data
    ok.value = '图像参数已保存并应用'
  } catch (e) {
    error.value = String(e)
  } finally {
    savingImage.value = false
  }
}

async function onSaveEncode() {
  error.value = ''
  ok.value = ''
  if (
    !window.confirm(
      '修改码流参数可能导致预览短暂中断。分辨率变更会重建管线。确认继续？',
    )
  ) {
    return
  }
  savingEncode.value = true
  try {
    const r = await setVideoEncode({ ...encode.value, apply: true })
    if (r.code !== 0) {
      error.value = r.msg || '保存编码参数失败'
      return
    }
    encode.value = r.data
    ok.value = encode.value.stream_up
      ? '编码参数已保存并应用到当前流'
      : '编码参数已保存（预览连接后生效）'
  } catch (e) {
    error.value = String(e)
  } finally {
    savingEncode.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <section class="video">
    <h2>图像与视频</h2>
    <p v-if="loading" class="muted">加载中…</p>
    <template v-else>
      <div class="panel">
        <h3>图像调节</h3>
        <label>
          亮度 {{ image.brightness }}
          <input v-model.number="image.brightness" type="range" min="0" max="100" />
        </label>
        <label>
          对比度 {{ image.contrast }}
          <input v-model.number="image.contrast" type="range" min="0" max="100" />
        </label>
        <label>
          饱和度 {{ image.saturation }}
          <input v-model.number="image.saturation" type="range" min="0" max="100" />
        </label>
        <label class="row-check">
          <input v-model="image.mirror" type="checkbox" /> 水平镜像
        </label>
        <label class="row-check">
          <input v-model="image.flip" type="checkbox" /> 垂直翻转
        </label>
        <button type="button" :disabled="savingImage" @click="onSaveImage">
          {{ savingImage ? '…' : '应用图像参数' }}
        </button>
      </div>

      <div class="panel">
        <h3>码流参数</h3>
        <p class="muted">流状态：{{ encode.stream_up ? '运行中' : '未起流' }}</p>
        <fieldset>
          <legend>主码流 (H.265)</legend>
          <label>宽 <input v-model.number="encode.main.w" type="number" /></label>
          <label>高 <input v-model.number="encode.main.h" type="number" /></label>
          <label>帧率 <input v-model.number="encode.main.fps" type="number" /></label>
          <label>码率 kbps <input v-model.number="encode.main.bitrate_kbps" type="number" /></label>
          <label>GOP <input v-model.number="encode.main.gop" type="number" /></label>
        </fieldset>
        <fieldset>
          <legend>子码流 (H.264 / 预览)</legend>
          <label>宽 <input v-model.number="encode.sub.w" type="number" /></label>
          <label>高 <input v-model.number="encode.sub.h" type="number" /></label>
          <label>帧率 <input v-model.number="encode.sub.fps" type="number" /></label>
          <label>码率 kbps <input v-model.number="encode.sub.bitrate_kbps" type="number" /></label>
          <label>GOP <input v-model.number="encode.sub.gop" type="number" /></label>
        </fieldset>
        <button type="button" :disabled="savingEncode" @click="onSaveEncode">
          {{ savingEncode ? '…' : '保存并应用码流' }}
        </button>
      </div>

      <p v-if="error" class="error">{{ error }}</p>
      <p v-if="ok" class="muted">{{ ok }}</p>
      <button type="button" class="ghost" @click="refresh">刷新</button>
    </template>
  </section>
</template>

<style scoped>
.video { max-width: 520px; display: grid; gap: 1rem; }
.panel { display: grid; gap: 0.75rem; padding: 0.75rem 0; border-top: 1px solid var(--border, #333); }
.panel:first-of-type { border-top: none; }
h3 { margin: 0; font-size: 1rem; }
label { display: grid; gap: 0.35rem; color: var(--muted); font-size: 0.9rem; }
.row-check { display: flex; align-items: center; gap: 0.5rem; }
fieldset { border: 1px solid var(--border, #333); border-radius: 6px; display: grid; gap: 0.5rem; padding: 0.75rem; }
legend { padding: 0 0.25rem; color: var(--muted); font-size: 0.85rem; }
button.ghost {
  background: transparent;
  border: 1px solid var(--border, #444);
  color: inherit;
  width: fit-content;
}
</style>
