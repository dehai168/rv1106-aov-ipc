<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { getNetwork, setNetwork, type NetworkConfig } from '../api'
import PageShell from '../components/PageShell.vue'

const loading = ref(true)
const saving = ref(false)
const activeTab = ref('status')
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

const IPV4_RE =
  /^(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)(\.(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)){3}$/

function isIpv4(v: string) {
  return IPV4_RE.test(v.trim())
}

function isValidNetmask(v: string) {
  if (!isIpv4(v)) return false
  const parts = v.split('.').map((x) => parseInt(x, 10))
  let bits = 0
  for (const p of parts) bits = (bits << 8) | p
  bits = bits >>> 0
  if (bits === 0) return false
  const inv = (~bits) >>> 0
  return ((inv + 1) & inv) === 0
}

function validateParams(): string | null {
  if (form.value.mode === 'static') {
    if (!isIpv4(form.value.ip)) return '请输入合法的 IP 地址'
    if (!isValidNetmask(form.value.netmask)) return '请输入合法的子网掩码'
    if (!isIpv4(form.value.gateway)) return '请输入合法的网关地址'
  }
  if (form.value.dns1 && !isIpv4(form.value.dns1)) return 'DNS1 格式不正确'
  if (form.value.dns2 && !isIpv4(form.value.dns2)) return 'DNS2 格式不正确'
  return null
}

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
  const err = validateParams()
  if (err) {
    ElMessage.warning(err)
    return
  }
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
    activeTab.value = 'status'
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    saving.value = false
  }
}

onMounted(refresh)
</script>

<template>
  <PageShell title="网络" subtitle="查看链路状态并配置 eth0（有线）。">
    <div v-loading="loading">
      <el-tabs v-model="activeTab">
        <el-tab-pane label="状态" name="status">
          <el-descriptions class="ipc-desc" :column="1" border>
            <el-descriptions-item label="接口">{{ form.iface || 'eth0' }}</el-descriptions-item>
            <el-descriptions-item label="模式">
              {{ form.mode === 'static' ? '静态 IP' : form.mode === 'dhcp' ? 'DHCP' : (form.mode || '-') }}
            </el-descriptions-item>
            <el-descriptions-item label="链路">{{ form.link || '-' }}</el-descriptions-item>
            <el-descriptions-item label="当前 IP">{{ form.current_ip || '(无)' }}</el-descriptions-item>
            <el-descriptions-item label="当前掩码">{{ form.current_netmask || '-' }}</el-descriptions-item>
            <el-descriptions-item label="当前网关">{{ form.current_gateway || '-' }}</el-descriptions-item>
            <el-descriptions-item label="usb0">{{ form.usb0_ip || '-' }}</el-descriptions-item>
          </el-descriptions>
        </el-tab-pane>

        <el-tab-pane label="参数" name="params">
          <el-form label-width="100px" style="max-width: 520px">
            <el-form-item label="模式">
              <el-radio-group v-model="form.mode">
                <el-radio-button label="dhcp">DHCP</el-radio-button>
                <el-radio-button label="static">静态 IP</el-radio-button>
              </el-radio-group>
            </el-form-item>
            <template v-if="form.mode === 'static'">
              <el-form-item label="IP" required>
                <el-input v-model="form.ip" placeholder="例如 192.168.1.100" />
              </el-form-item>
              <el-form-item label="子网掩码" required>
                <el-input v-model="form.netmask" placeholder="例如 255.255.255.0" />
              </el-form-item>
              <el-form-item label="网关" required>
                <el-input v-model="form.gateway" placeholder="例如 192.168.1.1" />
              </el-form-item>
            </template>
            <el-form-item label="DNS1"><el-input v-model="form.dns1" placeholder="可选，如 8.8.8.8" /></el-form-item>
            <el-form-item label="DNS2"><el-input v-model="form.dns2" placeholder="可选" /></el-form-item>
            <el-form-item>
              <el-button type="primary" :loading="saving" @click="onSave">保存并应用</el-button>
            </el-form-item>
          </el-form>
        </el-tab-pane>
      </el-tabs>
    </div>
  </PageShell>
</template>
