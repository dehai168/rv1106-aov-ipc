import { ref, watch } from 'vue'
import { getDeviceName, getSystemInfo } from '../api'

const deviceName = ref('AOV IPC')
let loading: Promise<void> | null = null

function syncDocumentTitle(name: string) {
  if (typeof document !== 'undefined') {
    document.title = name?.trim() || 'AOV IPC'
  }
}

watch(deviceName, (v) => syncDocumentTitle(v), { immediate: true })

export function useDeviceName() {
  async function refreshDeviceName(preferAuth = false) {
    if (loading) return loading
    loading = (async () => {
      try {
        if (preferAuth) {
          const r = await getSystemInfo()
          if (r.code === 0 && r.data?.device_name) {
            deviceName.value = r.data.device_name
            return
          }
        }
        const r = await getDeviceName()
        if (r.code === 0 && r.data?.device_name) {
          deviceName.value = r.data.device_name
        }
      } catch {
        /* keep previous */
      } finally {
        loading = null
      }
    })()
    return loading
  }

  function setDeviceNameLocal(name: string) {
    if (name?.trim()) deviceName.value = name.trim()
  }

  return { deviceName, refreshDeviceName, setDeviceNameLocal }
}
