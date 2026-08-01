<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { getRecordConfig, setRecordConfig, type RecordConfig } from '../api'
import PageShell from '../components/PageShell.vue'
import ScheduleGrid from '../components/ScheduleGrid.vue'

const loading = ref(true)
const saving = ref(false)
const activeTab = ref('params')
const cfg = ref<RecordConfig | null>(null)

function emptyGrid() {
  return '1'.repeat(168)
}

function ensure(c: RecordConfig): RecordConfig {
  if (!c.mode) c.mode = 'motion'
  if (!c.schedule) c.schedule = { enabled: false, grid: emptyGrid() }
  if (!c.schedule.grid || c.schedule.grid.length !== 168) c.schedule.grid = emptyGrid()
  return c
}

const scheduleDisabled = computed(() => !cfg.value || cfg.value.mode !== 'schedule')

async function refresh() {
  loading.value = true
  try {
    const r = await getRecordConfig()
    if (r.code !== 0) throw new Error(r.msg || '读取录像计划失败')
    cfg.value = ensure(r.data)
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onSave() {
  if (!cfg.value) return
  saving.value = true
  try {
    const r = await setRecordConfig({
      mode: cfg.value.mode,
      segment_sec: cfg.value.segment_sec,
      pre_record_sec: cfg.value.pre_record_sec,
      quiet_sec: cfg.value.quiet_sec,
      recycle_free_percent: cfg.value.recycle_free_percent,
      schedule: cfg.value.schedule,
      apply: true,
    })
    if (r.code !== 0) {
      ElMessage.error(r.msg || '保存失败')
      return
    }
    cfg.value = ensure(r.data)
    ElMessage.success(
      cfg.value.mode === 'motion'
        ? '已保存（事件模式：打开实时预览后生效）'
        : '已保存并应用',
    )
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    saving.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <PageShell title="录像计划" subtitle="配置录像模式、分段参数与定时计划。">
    <div v-loading="loading">
      <template v-if="cfg">
        <div class="runtime">
          <el-tag :type="cfg.running ? 'success' : 'info'">
            {{ cfg.running ? '录像中' : '未录像' }}
          </el-tag>
          <span class="muted">运行模式 {{ cfg.runtime_mode || '-' }}</span>
          <el-tag v-if="cfg.policy_hold" size="small" type="warning">媒体常驻</el-tag>
        </div>

        <el-tabs v-model="activeTab">
          <el-tab-pane label="录像参数" name="params">
            <el-form label-width="140px" style="max-width: 560px">
              <el-form-item label="录像模式">
                <el-radio-group v-model="cfg.mode">
                  <el-radio-button label="motion">事件（运动）</el-radio-button>
                  <el-radio-button label="continuous">连续</el-radio-button>
                  <el-radio-button label="schedule">定时</el-radio-button>
                </el-radio-group>
              </el-form-item>
              <el-form-item label="分段时长(秒)">
                <el-input-number v-model="cfg.segment_sec" :min="10" :max="600" />
              </el-form-item>
              <el-form-item label="预录(秒)">
                <el-input-number v-model="cfg.pre_record_sec" :min="1" :max="30" />
              </el-form-item>
              <el-form-item label="延录/静音(秒)">
                <el-input-number v-model="cfg.quiet_sec" :min="5" :max="600" />
                <div class="hint">事件模式：运动停止后继续录制的时长</div>
              </el-form-item>
            </el-form>
          </el-tab-pane>

          <el-tab-pane label="定时录像计划" name="plan">
            <el-alert
              class="mb"
              title="仅定时模式下生效；关闭格子过滤则定时模式内全天录。"
              type="info"
              show-icon
              :closable="false"
            />
            <el-form label-width="140px">
              <el-form-item label="启用格子过滤">
                <el-switch
                  v-model="cfg.schedule.enabled"
                  :disabled="scheduleDisabled"
                  active-text="关=定时模式内全天录"
                />
              </el-form-item>
              <el-form-item label-width="0">
                <ScheduleGrid
                  v-model="cfg.schedule.grid"
                  class="full"
                  :disabled="scheduleDisabled || !cfg.schedule.enabled"
                />
              </el-form-item>
            </el-form>
          </el-tab-pane>
        </el-tabs>

        <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
      </template>
    </div>
  </PageShell>
</template>

<style scoped>
.runtime {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 0.75rem;
  flex-wrap: wrap;
}
.muted { color: var(--ipc-muted); font-size: 0.9rem; }
.hint { color: var(--ipc-muted); font-size: 0.8rem; margin-left: 0.5rem; }
.full { width: 100%; }
.mb { margin-bottom: 1rem; }
</style>
