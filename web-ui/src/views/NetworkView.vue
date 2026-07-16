<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { getNetwork, setNetwork, type NetworkConfig } from '../api'

const loading = ref(true)
const saving = ref(false)
const error = ref('')
const ok = ref('')
const form = ref<NetworkConfig>({
  iface: 'eth0',
  mode: 'dhcp',
  ip: '',
  netmask: '255.255.255.0',
  gateway: '',
  dns1: '',
  dns2: '',
  link: '',
  current_ip: '',
  current_netmask: '',
  current_gateway: '',
  usb0_ip: '',
})

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    const r = await getNetwork()
    if (r.code !== 0) {
      error.value = r.msg || '读取失败'
      return
    }
    form.value = { ...r.data }
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}

async function onSave() {
  error.value = ''
  ok.value = ''
  if (form.value.mode === 'static') {
    const tip =
      '将立即改写 eth0 地址。若填错 IP，可能无法再从有线口访问（USB/adb 通常仍可用）。确认继续？'
    if (!window.confirm(tip)) return
  } else if (!window.confirm('将对 eth0 执行 DHCP 获取，确认继续？')) {
    return
  }
  saving.value = true
  try {
    const r = await setNetwork({
      iface: form.value.iface || 'eth0',
      mode: form.value.mode,
      ip: form.value.ip,
      netmask: form.value.netmask,
      gateway: form.value.gateway,
      dns1: form.value.dns1,
      dns2: form.value.dns2,
      apply: true,
    })
    if (r.code !== 0) {
      error.value = r.msg || '保存失败'
      return
    }
    form.value = { ...r.data }
    ok.value = '已保存并尝试生效'
  } catch (e) {
    error.value = String(e)
  } finally {
    saving.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <section class="net">
    <h2>网络参数</h2>
    <p class="muted">配置 eth0（有线）。usb0 仅供调试展示，本页不修改。</p>
    <p v-if="loading" class="muted">加载中…</p>
    <div v-else class="panel">
      <p class="status">
        链路 <b>{{ form.link || '-' }}</b> · 当前 IP
        <b>{{ form.current_ip || '(无)' }}</b>
        <span v-if="form.current_gateway"> · 网关 {{ form.current_gateway }}</span>
      </p>
      <p class="muted">usb0: {{ form.usb0_ip || '-' }}</p>

      <label>
        模式
        <select v-model="form.mode">
          <option value="dhcp">DHCP</option>
          <option value="static">静态 IP</option>
        </select>
      </label>

      <template v-if="form.mode === 'static'">
        <label>IP <input v-model="form.ip" /></label>
        <label>子网掩码 <input v-model="form.netmask" /></label>
        <label>网关 <input v-model="form.gateway" /></label>
      </template>

      <label>DNS1 <input v-model="form.dns1" placeholder="可选" /></label>
      <label>DNS2 <input v-model="form.dns2" placeholder="可选" /></label>

      <p v-if="error" class="error">{{ error }}</p>
      <p v-if="ok" class="muted">{{ ok }}</p>
      <div class="row">
        <button type="button" :disabled="saving" @click="onSave">
          {{ saving ? '…' : '保存并应用' }}
        </button>
        <button type="button" class="ghost" :disabled="saving" @click="refresh">刷新</button>
      </div>
    </div>
  </section>
</template>

<style scoped>
.net { max-width: 480px; display: grid; gap: 0.75rem; }
.panel { display: grid; gap: 0.75rem; }
label { display: grid; gap: 0.35rem; color: var(--muted); font-size: 0.9rem; }
.status { font-size: 0.95rem; }
.row { display: flex; gap: 0.75rem; flex-wrap: wrap; }
button.ghost {
  background: transparent;
  border: 1px solid var(--border, #444);
  color: inherit;
}
</style>
