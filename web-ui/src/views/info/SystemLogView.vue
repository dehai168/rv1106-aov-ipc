<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { getSystemLogTail, systemLogUrl } from '../../api'
import { Document, Download, Refresh } from '@element-plus/icons-vue'

const logUrl = systemLogUrl()
const loading = ref(false)
const auto = ref(false)
const lines = ref<string[]>([])
const text = ref('')
let timer: number | undefined

async function refresh() {
  loading.value = true
  try {
    const r = await getSystemLogTail(200)
    if (r.code !== 0) {
      ElMessage.error(r.msg || '读取日志失败')
      return
    }
    lines.value = r.data.lines || []
    text.value = lines.value.join('\n')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

function onAutoChange(v: string | number | boolean) {
  const on = !!v
  if (timer) {
    window.clearInterval(timer)
    timer = undefined
  }
  if (on) {
    timer = window.setInterval(refresh, 5000)
  }
}

onMounted(refresh)
onUnmounted(() => {
  if (timer) window.clearInterval(timer)
})
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">系统日志</h2>
      <p class="page-desc">查看 ipc_app.log 尾部，或下载完整日志文件。</p>
    </div>

    <el-card shadow="never" class="panel-card">
      <div class="tools">
        <el-button type="primary" :icon="Refresh" :loading="loading" @click="refresh">刷新尾部</el-button>
        <el-switch v-model="auto" active-text="每 5s 自动刷新" @change="onAutoChange" />
        <a :href="logUrl" target="_blank" rel="noopener">
          <el-button :icon="Download">下载完整日志</el-button>
        </a>
      </div>
      <el-input
        v-model="text"
        type="textarea"
        :rows="22"
        readonly
        class="log-box"
        placeholder="暂无日志内容"
      />
      <p class="hint">
        <el-icon><Document /></el-icon>
        最近 {{ lines.length }} 行
      </p>
    </el-card>
  </section>
</template>

<style scoped>
.tools {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.8rem;
  margin-bottom: 0.8rem;
}
.log-box :deep(textarea) {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 12px;
  line-height: 1.45;
}
.hint {
  margin-top: 0.5rem;
  color: var(--ipc-muted);
  display: flex;
  align-items: center;
  gap: 0.35rem;
  font-size: 13px;
}
</style>
