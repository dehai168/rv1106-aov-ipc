<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
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
const activeTab = ref('image')
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
  try {
    const [img, enc] = await Promise.all([getVideoImage(), getVideoEncode()])
    if (img.code !== 0) {
      ElMessage.error(img.msg || '读取图像参数失败')
      return
    }
    if (enc.code !== 0) {
      ElMessage.error(enc.msg || '读取编码参数失败')
      return
    }
    image.value = img.data
    encode.value = enc.data
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onSaveImage() {
  savingImage.value = true
  try {
    const r = await setVideoImage({ ...image.value, apply: true })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存图像参数失败')
      return
    }
    image.value = r.data
    ElMessage.success('图像参数已保存并应用')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    savingImage.value = false
  }
}

async function onSaveEncode() {
  try {
    await ElMessageBox.confirm(
      '修改码流参数可能导致预览短暂中断。分辨率变更会重建管线。确认继续？',
      '确认修改码流',
      { type: 'warning', confirmButtonText: '继续', cancelButtonText: '取消' },
    )
  } catch {
    return
  }
  savingEncode.value = true
  try {
    const r = await setVideoEncode({ ...encode.value, apply: true })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存编码参数失败')
      return
    }
    encode.value = r.data
    ElMessage.success(
      encode.value.stream_up ? '编码参数已保存并应用到当前流' : '编码参数已保存（预览连接后生效）',
    )
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    savingEncode.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">图像与码流</h2>
      <p class="page-desc">调节画面效果与主/子码流编码参数。</p>
    </div>

    <el-card v-loading="loading" shadow="never" class="panel-card">
      <el-tabs v-model="activeTab">
        <el-tab-pane label="图像" name="image">
          <el-form label-width="100px" style="max-width: 520px">
            <el-form-item :label="`亮度 ${image.brightness}`">
              <el-slider v-model="image.brightness" :min="0" :max="100" />
            </el-form-item>
            <el-form-item :label="`对比度 ${image.contrast}`">
              <el-slider v-model="image.contrast" :min="0" :max="100" />
            </el-form-item>
            <el-form-item :label="`饱和度 ${image.saturation}`">
              <el-slider v-model="image.saturation" :min="0" :max="100" />
            </el-form-item>
            <el-form-item label="镜像/翻转">
              <el-space>
                <el-switch v-model="image.mirror" active-text="水平镜像" />
                <el-switch v-model="image.flip" active-text="垂直翻转" />
              </el-space>
            </el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="savingImage" @click="onSaveImage">应用图像参数</el-button>
            </el-form-item>
          </el-form>
        </el-tab-pane>

        <el-tab-pane label="码流" name="stream">
          <el-alert
            class="mb"
            :title="`流状态：${encode.stream_up ? '运行中' : '未起流'}`"
            :type="encode.stream_up ? 'success' : 'info'"
            show-icon
            :closable="false"
          />
          <el-row :gutter="16">
            <el-col :xs="24" :md="12">
              <h3 class="sub">主码流 (H.265)</h3>
              <el-form label-width="90px">
                <el-form-item label="宽"><el-input-number v-model="encode.main.w" :min="320" :step="16" /></el-form-item>
                <el-form-item label="高"><el-input-number v-model="encode.main.h" :min="240" :step="16" /></el-form-item>
                <el-form-item label="帧率"><el-input-number v-model="encode.main.fps" :min="1" :max="30" /></el-form-item>
                <el-form-item label="码率"><el-input-number v-model="encode.main.bitrate_kbps" :min="64" :step="64" /> <span class="unit">kbps</span></el-form-item>
                <el-form-item label="GOP"><el-input-number v-model="encode.main.gop" :min="1" /></el-form-item>
              </el-form>
            </el-col>
            <el-col :xs="24" :md="12">
              <h3 class="sub">子码流 (H.264 / 预览)</h3>
              <el-form label-width="90px">
                <el-form-item label="宽"><el-input-number v-model="encode.sub.w" :min="320" :step="16" /></el-form-item>
                <el-form-item label="高"><el-input-number v-model="encode.sub.h" :min="240" :step="16" /></el-form-item>
                <el-form-item label="帧率"><el-input-number v-model="encode.sub.fps" :min="1" :max="30" /></el-form-item>
                <el-form-item label="码率"><el-input-number v-model="encode.sub.bitrate_kbps" :min="64" :step="64" /> <span class="unit">kbps</span></el-form-item>
                <el-form-item label="GOP"><el-input-number v-model="encode.sub.gop" :min="1" /></el-form-item>
              </el-form>
            </el-col>
          </el-row>
          <el-button type="primary" :loading="savingEncode" @click="onSaveEncode">保存并应用码流</el-button>
          <el-button @click="refresh">刷新</el-button>
        </el-tab-pane>
      </el-tabs>
    </el-card>
  </section>
</template>

<style scoped>
.mb { margin-bottom: 1rem; }
.sub { margin: 0 0 0.75rem; font-size: 0.95rem; color: var(--ipc-muted); }
.unit { margin-left: 0.35rem; color: var(--ipc-muted); font-size: 0.85rem; }
</style>
