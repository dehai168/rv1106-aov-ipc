<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue'
import { getAlarmEvents, getAlarmMotion, setAlarmMotion, type AlarmEvent } from '../api'

const loading = ref(true)
const saving = ref(false)
const error = ref('')
const ok = ref('')
let refreshTimer: number | undefined

const motion = ref<{
  enabled: boolean
  sensitivity: number
  square_pct: number
  hit_frames: number
  running: boolean
  motion_count: number
  last_event: AlarmEvent
} | null>(null)

const events = ref<AlarmEvent[]>([])
const eventsLimit = ref(50)

async function refresh() {
  loading.value = true
  error.value = ''
  ok.value = ''
  try {
    const [m, evs] = await Promise.all([
      getAlarmMotion(),
      getAlarmEvents(eventsLimit.value),
    ])
    if (m.code !== 0) throw new Error(m.msg || '读取告警配置失败')
    if (evs.code !== 0) throw new Error(evs.msg || '读取告警事件失败')
    motion.value = m.data
    events.value = evs.data.events
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}

async function onSave() {
  if (!motion.value) return
  saving.value = true
  error.value = ''
  ok.value = ''
  try {
    const r = await setAlarmMotion({
      enabled: motion.value.enabled,
      sensitivity: motion.value.sensitivity,
      square_pct: motion.value.square_pct,
      hit_frames: motion.value.hit_frames,
      apply: true,
    })
    if (r.code !== 0) {
      error.value = r.msg || '保存失败'
      return
    }
    ok.value = '已保存（如需立即生效，请保持实时预览页已打开）'
    await refresh()
  } catch (e) {
    error.value = String(e)
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  refresh()
  refreshTimer = window.setInterval(() => {
    getAlarmEvents(eventsLimit.value).then((evs) => {
      if (evs.code === 0) events.value = evs.data.events
    }).catch(() => {
      /* ignore poll errors */
    })
    getAlarmMotion().then((m) => {
      if (m.code === 0 && motion.value) {
        motion.value.running = m.data.running
        motion.value.motion_count = m.data.motion_count
        motion.value.last_event = m.data.last_event
      }
    }).catch(() => {
      /* ignore */
    })
  }, 3000)
})

onUnmounted(() => {
  if (refreshTimer) window.clearInterval(refreshTimer)
})
</script>

<template>
  <section class="alarm">
    <h2>告警与智能分析</h2>
    <p class="muted tip">
      保持「实时预览」打开约 3 秒后自动启动移动侦测；本页每 3 秒刷新事件。走动后看下方列表。
    </p>
    <p v-if="loading" class="muted">加载中…</p>
    <div v-else class="grid">
      <div class="panel">
        <h3>检测配置</h3>
        <label class="row-check">
          <input type="checkbox" v-model="motion!.enabled" />
          启用运动侦测
        </label>
        <label>
          灵敏度 (0-4)
          <input v-model.number="motion!.sensitivity" type="number" min="0" max="4" />
        </label>
        <label>
          square_pct (阈值)
          <input v-model.number="motion!.square_pct" type="number" min="1" />
        </label>
        <label>
          连续命中帧 hit_frames
          <input v-model.number="motion!.hit_frames" type="number" min="1" />
        </label>
        <div class="row">
          <button type="button" :disabled="saving" @click="onSave">
            {{ saving ? '…' : '保存并应用' }}
          </button>
          <button type="button" class="ghost" :disabled="saving" @click="refresh">刷新</button>
        </div>

        <p class="muted">
          运行中：<b>{{ motion!.running ? '是' : '否' }}</b> · MotionCount：<b>{{ motion!.motion_count }}</b>
        </p>
        <p v-if="motion!.last_event" class="muted">
          最近事件：ts={{ motion!.last_event.ts }} · square={{ motion!.last_event.square }} · pct_x10={{ motion!.last_event.pct_x10 }}
        </p>
      </div>

      <div class="panel">
        <h3>事件列表</h3>
        <label>
          显示最近 N 条
          <input v-model.number="eventsLimit" type="number" min="1" max="200" />
        </label>
        <div class="row">
          <button type="button" class="ghost" @click="refresh">刷新事件</button>
        </div>
        <div class="events">
          <div v-if="events.length === 0" class="muted">暂无事件</div>
          <div v-for="e in events" :key="e.ts" class="event">
            <div class="line">
              <b>{{ new Date(e.ts * 1000).toLocaleString() }}</b>
              · square={{ e.square }}
              · pct_x10={{ e.pct_x10 }}
            </div>
            <div class="muted">rect=[{{ e.rect[0] }},{{ e.rect[1] }},{{ e.rect[2] }},{{ e.rect[3] }}]</div>
          </div>
        </div>
      </div>
    </div>

    <p v-if="error" class="error">{{ error }}</p>
    <p v-if="ok" class="muted">{{ ok }}</p>
  </section>
</template>

<style scoped>
.alarm { max-width: 980px; display: grid; gap: 1rem; }
.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; }
.panel { border: 1px solid var(--border); border-radius: 8px; padding: 0.85rem; display: grid; gap: 0.65rem; }
h3 { margin: 0; font-size: 1rem; color: var(--text); }
label { display: grid; gap: 0.35rem; color: var(--muted); font-size: 0.9rem; }
input { padding: 0.35rem 0.5rem; }
.row { display: flex; gap: 0.75rem; flex-wrap: wrap; align-items: center; }
.row-check { display: flex; flex-direction: row; align-items: center; gap: 0.5rem; }
.ghost { background: transparent; border: 1px solid var(--border); color: inherit; }
.events { max-height: 460px; overflow: auto; display: grid; gap: 0.5rem; }
.event { border: 1px solid var(--border); border-radius: 8px; padding: 0.6rem; }
.line { font-size: 0.9rem; }
.tip { font-size: 0.85rem; margin: 0; }

@media (max-width: 860px) {
  .grid { grid-template-columns: 1fr; }
}
</style>

