<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { getNetwork, setNetwork, type NetworkConfig } from '../api'

const loading = ref(true)
const saving = ref(false)
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
  try {
    const r = await getNetwork()
    if (r.code !== 0) {
      ElMessage.error(r.msg || '读取失败')
      return
    }
    form.value = { ...r.data }
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}

async function onSave() {
  try {
    if (form.value.mode === 'static') {
      await ElMessageBox.confirm(
        '将立即改写 eth0 地址。若填错 IP，可能无法再从有线口访问（USB/adb 通常仍可用）。确认继续？',
        '确认修改网络',
        { type: 'warning', confirmButtonText: '继续', cancelButtonText: '取消' },
      )
    } else {
      await ElMessageBox.confirm('将对 eth0 执行 DHCP 获取，确认继续？', '确认修改网络', {
        type: 'warning',
        confirmButtonText: '继续',
        cancelButtonText: '取消',
      })
    }
  } catch {
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
      ElMessage.error(r.msg || '保存失败')
      return
    }
    form.value = { ...r.data }
    ElMessage.success('已保存并尝试生效')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    saving.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <section class="page">
    <div>
      <h2 class="page-title">网络</h2>
      <p class="page-desc">配置 eth0（有线）。usb0 仅供调试展示，本页不修改。</p>
    </div>

    <el-card v-loading="loading" shadow="never" class="panel-card" style="max-width: 640px">
      <el-descriptions :column="1" border class="mb">
        <el-descriptions-item label="链路">{{ form.link || '-' }}</el-descriptions-item>
        <el-descriptions-item label="当前 IP">{{ form.current_ip || '(无)' }}</el-descriptions-item>
        <el-descriptions-item label="当前网关">{{ form.current_gateway || '-' }}</el-descriptions-item>
        <el-descriptions-item label="usb0">{{ form.usb0_ip || '-' }}</el-descriptions-item>
      </el-descriptions>

      <el-form label-width="100px">
        <el-form-item label="模式">
          <el-radio-group v-model="form.mode">
            <el-radio-button label="dhcp">DHCP</el-radio-button>
            <el-radio-button label="static">静态 IP</el-radio-button>
          </el-radio-group>
        </el-form-item>
        <template v-if="form.mode === 'static'">
          <el-form-item label="IP"><el-input v-model="form.ip" /></el-form-item>
          <el-form-item label="子网掩码"><el-input v-model="form.netmask" /></el-form-item>
          <el-form-item label="网关"><el-input v-model="form.gateway" /></el-form-item>
        </template>
        <el-form-item label="DNS1"><el-input v-model="form.dns1" placeholder="可选" /></el-form-item>
        <el-form-item label="DNS2"><el-input v-model="form.dns2" placeholder="可选" /></el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
          <el-button :disabled="saving" @click="refresh">刷新</el-button>
        </el-form-item>
      </el-form>
    </el-card>
  </section>
</template>

<style scoped>
.mb { margin-bottom: 1.25rem; }
</style>
