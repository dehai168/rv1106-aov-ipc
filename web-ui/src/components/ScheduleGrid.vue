<script setup lang="ts">
import { computed } from 'vue'

const DAY_LABELS = ['一', '二', '三', '四', '五', '六', '日'] as const
const HOURS = Array.from({ length: 24 }, (_, i) => i)

const props = defineProps<{
  modelValue: string
  disabled?: boolean
}>()

const emit = defineEmits<{
  'update:modelValue': [string]
}>()

function emptyGrid(fill: '0' | '1' = '1') {
  return fill.repeat(168)
}

function ensureGrid(raw: string | undefined): string {
  if (raw && raw.length === 168 && /^[01]+$/.test(raw)) return raw
  return emptyGrid('1')
}

const cells = computed(() => ensureGrid(props.modelValue))

function setCell(day: number, hour: number, on: boolean) {
  if (props.disabled) return
  const arr = cells.value.split('')
  arr[day * 24 + hour] = on ? '1' : '0'
  emit('update:modelValue', arr.join(''))
}

function isOn(day: number, hour: number) {
  return cells.value[day * 24 + hour] === '1'
}

let painting: boolean | null = null

function onDown(day: number, hour: number, e: PointerEvent) {
  if (props.disabled) return
  ;(e.currentTarget as HTMLElement).setPointerCapture?.(e.pointerId)
  painting = !isOn(day, hour)
  setCell(day, hour, painting)
}

function onEnter(day: number, hour: number) {
  if (painting === null || props.disabled) return
  setCell(day, hour, painting)
}

function onUp() {
  painting = null
}

function fillAll(on: boolean) {
  if (props.disabled) return
  emit('update:modelValue', emptyGrid(on ? '1' : '0'))
}

function fillWorkdayDaytime() {
  if (props.disabled) return
  const arr = emptyGrid('0').split('')
  for (let d = 0; d < 5; d++) {
    for (let h = 8; h < 20; h++) arr[d * 24 + h] = '1'
  }
  emit('update:modelValue', arr.join(''))
}
</script>

<template>
  <div class="sched" :class="{ disabled }" @pointerup="onUp" @pointerleave="onUp">
    <div class="tools">
      <el-button size="small" :disabled="disabled" @click="fillAll(true)">全选</el-button>
      <el-button size="small" :disabled="disabled" @click="fillAll(false)">全清</el-button>
      <el-button size="small" :disabled="disabled" @click="fillWorkdayDaytime">工作日 8–20</el-button>
      <span class="hint">拖拽涂抹 · 蓝=布防</span>
    </div>
    <div class="grid-wrap">
      <div class="hour-row">
        <div class="day-lab" />
        <div v-for="h in HOURS" :key="'h' + h" class="hour-lab">{{ h }}</div>
      </div>
      <div v-for="(lab, d) in DAY_LABELS" :key="lab" class="day-row">
        <div class="day-lab">周{{ lab }}</div>
        <button
          v-for="h in HOURS"
          :key="d + '-' + h"
          type="button"
          class="cell"
          :class="{ on: isOn(d, h) }"
          :disabled="disabled"
          @pointerdown="onDown(d, h, $event)"
          @pointerenter="onEnter(d, h)"
        />
      </div>
    </div>
  </div>
</template>

<style scoped>
.sched.disabled { opacity: 0.55; }
.tools {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  align-items: center;
  margin-bottom: 0.5rem;
}
.hint { color: var(--ipc-muted, #888); font-size: 0.8rem; }
.grid-wrap {
  overflow-x: auto;
  user-select: none;
  touch-action: none;
}
.hour-row, .day-row {
  display: grid;
  grid-template-columns: 42px repeat(24, 26px);
  gap: 3px;
  margin-bottom: 3px;
}
.day-lab, .hour-lab {
  font-size: 12px;
  color: var(--ipc-muted, #888);
  display: flex;
  align-items: center;
  justify-content: center;
}
.cell {
  width: 26px;
  height: 26px;
  padding: 0;
  border: 1px solid var(--ipc-border, #444);
  background: #f0f0f0;
  cursor: pointer;
  border-radius: 2px;
}
.cell.on {
  background: #3d8bfd;
  border-color: #2b6fd4;
}
.cell:disabled { cursor: not-allowed; }
</style>
