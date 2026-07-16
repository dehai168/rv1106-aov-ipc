import { createRouter, createWebHashHistory } from 'vue-router'
import LoginView from './views/LoginView.vue'
import ShellView from './views/ShellView.vue'
import PasswordView from './views/PasswordView.vue'
import PreviewView from './views/PreviewView.vue'
import NetworkView from './views/NetworkView.vue'
import VideoView from './views/VideoView.vue'
import StorageView from './views/StorageView.vue'
import AlarmView from './views/AlarmView.vue'
import SystemView from './views/SystemView.vue'
import { me } from './api'

const router = createRouter({
  history: createWebHashHistory(),
  routes: [
    { path: '/login', component: LoginView },
    {
      path: '/',
      component: ShellView,
      children: [
        { path: '', redirect: '/preview' },
        { path: 'preview', component: PreviewView },
        { path: 'network', component: NetworkView },
        { path: 'video', component: VideoView },
        { path: 'storage', component: StorageView },
        { path: 'alarm', component: AlarmView },
        { path: 'system', component: SystemView },
        { path: 'password', component: PasswordView },
      ],
    },
  ],
})

router.beforeEach(async (to) => {
  if (to.path === '/login') return true
  try {
    const r = await me()
    if (r.code !== 0) return '/login'
    if (r.data.must_change && to.path !== '/password') return '/password'
    return true
  } catch {
    return '/login'
  }
})

export default router
