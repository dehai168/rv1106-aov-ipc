import { createRouter, createWebHashHistory } from 'vue-router'
import AppLayout from './layouts/AppLayout.vue'
import LoginView from './views/LoginView.vue'
import PasswordView from './views/PasswordView.vue'
import PreviewView from './views/PreviewView.vue'
import NetworkView from './views/NetworkView.vue'
import VideoView from './views/VideoView.vue'
import StorageView from './views/StorageView.vue'
import AlarmView from './views/AlarmView.vue'
import SystemView from './views/SystemView.vue'
import DeviceInfoView from './views/info/DeviceInfoView.vue'
import SystemLogView from './views/info/SystemLogView.vue'
import { me } from './api'

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: '/login', component: LoginView },
    {
      path: '/',
      component: AppLayout,
      children: [
        { path: '', redirect: '/preview' },
        { path: 'preview', component: PreviewView },
        { path: 'info', redirect: '/info/device' },
        { path: 'info/device', component: DeviceInfoView },
        { path: 'info/log', component: SystemLogView },
        { path: 'settings', redirect: '/settings/camera' },
        { path: 'settings/camera', component: VideoView },
        { path: 'settings/event/motion', component: AlarmView },
        { path: 'settings/storage', component: StorageView },
        { path: 'settings/network', component: NetworkView },
        { path: 'settings/system', component: SystemView },
        { path: 'settings/system/password', component: PasswordView },
        /* legacy redirects */
        { path: 'network', redirect: '/settings/network' },
        { path: 'video', redirect: '/settings/camera' },
        { path: 'storage', redirect: '/settings/storage' },
        { path: 'alarm', redirect: '/settings/event/motion' },
        { path: 'system', redirect: '/settings/system' },
        { path: 'password', redirect: '/settings/system/password' },
      ],
    },
  ],
})

router.beforeEach(async (to) => {
  if (to.path === '/login') return true
  try {
    const r = await me()
    if (r.code !== 0) return '/login'
    if (r.data.must_change && !to.path.includes('/password') && to.path !== '/settings/system/password') {
      return '/settings/system/password'
    }
    return true
  } catch {
    return '/login'
  }
})

export default router
