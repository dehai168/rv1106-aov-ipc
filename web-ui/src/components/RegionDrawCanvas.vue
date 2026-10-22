<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import type { AlarmRegion } from '../api'

const props = withDefaults(
  defineProps<{
    modelValue: AlarmRegion[]
    imageUrl?: string
    logicW?: number
    logicH?: number
    max?: number
    /** When >= 0, only edit this region index. */
    activeIndex?: number
    /** Cap the canvas width (px). 0 = fill the container. */
    maxWidth?: number
  }>(),
  {
    logicW: 640,
    logicH: 360,
    max: 4,
    activeIndex: -1,
    maxWidth: 0,
  },
)

const emit = defineEmits<{
  'update:modelValue': [AlarmRegion[]]
}>()

const wrapEl = ref<HTMLElement | null>(null)
const canvasEl = ref<HTMLCanvasElement | null>(null)
const selected = ref(-1)
const bgImg = ref<HTMLImageElement | null>(null)

type DragMode = 'none' | 'draw' | 'move' | 'resize'
let dragMode: DragMode = 'none'
let startLx = 0
let startLy = 0
let orig: AlarmRegion | null = null
let resizeCorner = ''

const regions = computed({
  get: () => props.modelValue || [],
  set: (v: AlarmRegion[]) => emit('update:modelValue', v),
})

function clamp(v: number, lo: number, hi: number) {
  return Math.max(lo, Math.min(hi, v))
}

function normRect(x: number, y: number, w: number, h: number): AlarmRegion {
  let nx = x
  let ny = y
  let nw = w
  let nh = h
  if (nw < 0) {
    nx += nw
    nw = -nw
  }
  if (nh < 0) {
    ny += nh
    nh = -nh
  }
  nx = clamp(Math.round(nx), 0, props.logicW - 1)
  ny = clamp(Math.round(ny), 0, props.logicH - 1)
  nw = clamp(Math.round(nw), 1, props.logicW - nx)
  nh = clamp(Math.round(nh), 1, props.logicH - ny)
  return { enabled: true, x: nx, y: ny, w: nw, h: nh }
}

function cssToLogic(clientX: number, clientY: number) {
  const canvas = canvasEl.value
  if (!canvas) return { x: 0, y: 0 }
  const rect = canvas.getBoundingClientRect()
  const x = ((clientX - rect.left) / rect.width) * props.logicW
  const y = ((clientY - rect.top) / rect.height) * props.logicH
  return { x, y }
}

function hitTest(lx: number, ly: number): { idx: number; corner: string } {
  const list = regions.value
  for (let i = list.length - 1; i >= 0; i--) {
    const r = list[i]
    if (!r.enabled || r.w <= 0 || r.h <= 0) continue
    const pad = 8 * (props.logicW / Math.max(1, canvasEl.value?.clientWidth || props.logicW))
    const corners: [string, number, number][] = [
      ['nw', r.x, r.y],
      ['ne', r.x + r.w, r.y],
      ['sw', r.x, r.y + r.h],
      ['se', r.x + r.w, r.y + r.h],
    ]
    for (const [name, cx, cy] of corners) {
      if (Math.abs(lx - cx) <= pad && Math.abs(ly - cy) <= pad) {
        return { idx: i, corner: name }
      }
    }
    if (lx >= r.x && lx <= r.x + r.w && ly >= r.y && ly <= r.y + r.h) {
      return { idx: i, corner: '' }
    }
  }
  return { idx: -1, corner: '' }
}

function paint() {
  const canvas = canvasEl.value
  if (!canvas) return
  const dpr = window.devicePixelRatio || 1
  const cssW = canvas.clientWidth || 640
  const cssH = canvas.clientHeight || 360
  canvas.width = Math.round(cssW * dpr)
  canvas.height = Math.round(cssH * dpr)
  const ctx = canvas.getContext('2d')
  if (!ctx) return
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  ctx.clearRect(0, 0, cssW, cssH)

  if (bgImg.value) {
    ctx.drawImage(bgImg.value, 0, 0, cssW, cssH)
  } else {
    ctx.fillStyle = '#1a1a1a'
    ctx.fillRect(0, 0, cssW, cssH)
    ctx.fillStyle = '#666'
    ctx.font = '14px sans-serif'
    ctx.fillText('点击「刷新底图」或打开预览后抓图', 16, 28)
  }

  const sx = cssW / props.logicW
  const sy = cssH / props.logicH
  regions.value.forEach((r, i) => {
    if (props.activeIndex >= 0 && i !== props.activeIndex) return
    if (!r.enabled || r.w <= 0 || r.h <= 0) return
    const x = r.x * sx
    const y = r.y * sy
    const w = r.w * sx
    const h = r.h * sy
    const focus = i === selected.value || props.activeIndex === i
    ctx.strokeStyle = focus ? '#ff6b35' : '#3d8bfd'
    ctx.lineWidth = focus ? 2.5 : 2
    ctx.strokeRect(x, y, w, h)
    ctx.fillStyle = focus ? 'rgba(255,107,53,0.15)' : 'rgba(61,139,253,0.12)'
    ctx.fillRect(x, y, w, h)
    ctx.fillStyle = ctx.strokeStyle
    ctx.font = '12px sans-serif'
    ctx.fillText(`区域${i + 1}`, x + 4, y + 14)
    if (focus) {
      const hs = 6
      ;[
        [x, y],
        [x + w, y],
        [x, y + h],
        [x + w, y + h],
      ].forEach(([hx, hy]) => {
        ctx.fillRect(hx - hs / 2, hy - hs / 2, hs, hs)
      })
    }
  })
}

function onPointerDown(e: PointerEvent) {
  const canvas = canvasEl.value
  if (!canvas) return
  canvas.setPointerCapture(e.pointerId)
  const { x, y } = cssToLogic(e.clientX, e.clientY)
  const hit = hitTest(x, y)
  startLx = x
  startLy = y
  const locked = props.activeIndex >= 0 ? props.activeIndex : -1
  if (locked >= 0) {
    const r = regions.value[locked]
    if (r && r.enabled && r.w > 0 && r.h > 0) {
      const pad = 8 * (props.logicW / Math.max(1, canvas.clientWidth || props.logicW))
      const corners: [string, number, number][] = [
        ['nw', r.x, r.y],
        ['ne', r.x + r.w, r.y],
        ['sw', r.x, r.y + r.h],
        ['se', r.x + r.w, r.y + r.h],
      ]
      for (const [name, cx, cy] of corners) {
        if (Math.abs(x - cx) <= pad && Math.abs(y - cy) <= pad) {
          selected.value = locked
          dragMode = 'resize'
          resizeCorner = name
          orig = { ...r }
          paint()
          return
        }
      }
      if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
        selected.value = locked
        dragMode = 'move'
        orig = { ...r }
        paint()
        return
      }
    }
    selected.value = locked
    dragMode = 'draw'
    paint()
    return
  }
  if (hit.idx >= 0 && hit.corner) {
    selected.value = hit.idx
    dragMode = 'resize'
    resizeCorner = hit.corner
    orig = { ...regions.value[hit.idx] }
  } else if (hit.idx >= 0) {
    selected.value = hit.idx
    dragMode = 'move'
    orig = { ...regions.value[hit.idx] }
  } else {
    const active = regions.value.filter((r) => r.enabled && r.w > 0 && r.h > 0)
    if (active.length >= props.max) {
      selected.value = -1
      dragMode = 'none'
      paint()
      return
    }
    dragMode = 'draw'
    selected.value = -1
  }
  paint()
}

function onPointerMove(e: PointerEvent) {
  if (dragMode === 'none') return
  const { x, y } = cssToLogic(e.clientX, e.clientY)
  const list = regions.value.map((r) => ({ ...r }))
  if (dragMode === 'draw') {
    const rect = normRect(startLx, startLy, x - startLx, y - startLy)
    let idx =
      props.activeIndex >= 0
        ? props.activeIndex
        : list.findIndex((r) => !r.enabled || r.w <= 0 || r.h <= 0)
    if (idx < 0) idx = list.length < props.max ? list.length : -1
    if (idx < 0) return
    while (list.length <= idx) list.push({ enabled: false, x: 0, y: 0, w: 0, h: 0 })
    list[idx] = rect
    selected.value = idx
    regions.value = list.slice(0, props.max)
  } else if (dragMode === 'move' && orig && selected.value >= 0) {
    const dx = Math.round(x - startLx)
    const dy = Math.round(y - startLy)
    list[selected.value] = normRect(orig.x + dx, orig.y + dy, orig.w, orig.h)
    regions.value = list
  } else if (dragMode === 'resize' && orig && selected.value >= 0) {
    let x1 = orig.x
    let y1 = orig.y
    let x2 = orig.x + orig.w
    let y2 = orig.y + orig.h
    if (resizeCorner.includes('w')) x1 = x
    if (resizeCorner.includes('e')) x2 = x
    if (resizeCorner.includes('n')) y1 = y
    if (resizeCorner.includes('s')) y2 = y
    list[selected.value] = normRect(x1, y1, x2 - x1, y2 - y1)
    regions.value = list
  }
  paint()
}

function onPointerUp() {
  dragMode = 'none'
  orig = null
  paint()
}

function clearSelected() {
  const idx = props.activeIndex >= 0 ? props.activeIndex : selected.value
  if (idx < 0) return
  const list = regions.value.map((r) => ({ ...r }))
  list[idx] = { enabled: false, x: 0, y: 0, w: 0, h: 0 }
  regions.value = list
  selected.value = -1
  paint()
}

watch(
  () => props.activeIndex,
  (idx) => {
    selected.value = idx >= 0 ? idx : -1
    paint()
  },
)

function clearAll() {
  regions.value = Array.from({ length: props.max }, () => ({
    enabled: false,
    x: 0,
    y: 0,
    w: 0,
    h: 0,
  }))
  selected.value = -1
  paint()
}

watch(
  () => props.imageUrl,
  (url) => {
    if (!url) {
      bgImg.value = null
      paint()
      return
    }
    const img = new Image()
    img.onload = () => {
      bgImg.value = img
      paint()
    }
    img.onerror = () => {
      bgImg.value = null
      paint()
    }
    img.src = url
  },
  { immediate: true },
)

watch(
  () => props.modelValue,
  () => paint(),
  { deep: true },
)

let ro: ResizeObserver | undefined
onMounted(() => {
  paint()
  if (wrapEl.value) {
    ro = new ResizeObserver(() => paint())
    ro.observe(wrapEl.value)
  }
})
onUnmounted(() => ro?.disconnect())

defineExpose({ clearSelected, clearAll, selected })
</script>

<template>
  <div
    class="region-draw"
    :style="maxWidth > 0 ? { maxWidth: `${maxWidth}px` } : undefined"
  >
    <div ref="wrapEl" class="stage">
      <canvas
        ref="canvasEl"
        class="canvas"
        @pointerdown="onPointerDown"
        @pointermove="onPointerMove"
        @pointerup="onPointerUp"
        @pointercancel="onPointerUp"
      />
    </div>
    <div class="toolbar">
      <el-button size="small" @click="clearSelected">
        {{ activeIndex >= 0 ? `清除区域${activeIndex + 1}` : '删除选中' }}
      </el-button>
      <el-button v-if="activeIndex < 0" size="small" @click="clearAll">清空全部</el-button>
      <span class="hint">
        {{
          activeIndex >= 0
            ? `正在编辑区域${activeIndex + 1} · 拖拽画框 / 移动 / 缩放`
            : `最多 ${max} 区 · 拖拽画框 · 选中后可移动/缩放`
        }}
      </span>
    </div>
  </div>
</template>

<style scoped>
.region-draw { width: 100%; }
.stage {
  width: 100%;
  aspect-ratio: 16 / 9;
  background: #111;
  border: 1px solid var(--ipc-border, #333);
  overflow: hidden;
  touch-action: none;
}
.canvas {
  width: 100%;
  height: 100%;
  display: block;
  cursor: crosshair;
}
.toolbar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.5rem;
  margin-top: 0.5rem;
}
.hint { color: var(--ipc-muted, #888); font-size: 0.8rem; }
</style>
