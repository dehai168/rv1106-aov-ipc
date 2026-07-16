<script setup lang="ts">
import { useRouter } from 'vue-router'
import { logout } from '../api'

const router = useRouter()
const nav = [
  { to: '/preview', label: '实时预览' },
  { to: '/network', label: '网络参数' },
  { to: '/video', label: '图像与视频' },
  { to: '/storage', label: '存储与回放' },
  { to: '/alarm', label: '告警与智能' },
  { to: '/system', label: '系统管理' },
]

async function onLogout() {
  await logout()
  router.replace('/login')
}
</script>

<template>
  <div class="shell">
    <aside>
      <div class="brand">AOV IPC</div>
      <nav>
        <router-link v-for="n in nav" :key="n.to" :to="n.to">{{ n.label }}</router-link>
      </nav>
      <button class="secondary logout" @click="onLogout">退出</button>
    </aside>
    <main>
      <router-view />
    </main>
  </div>
</template>

<style scoped>
.shell { display: grid; grid-template-columns: 220px 1fr; height: 100%; }
aside {
  background: var(--panel);
  border-right: 1px solid var(--border);
  padding: 1rem;
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
}
.brand { font-weight: 700; margin-bottom: 0.75rem; }
nav { display: grid; gap: 0.25rem; flex: 1; }
nav a {
  padding: 0.55rem 0.7rem;
  border-radius: 6px;
  color: var(--muted);
}
nav a.router-link-active {
  background: rgba(61, 139, 253, 0.15);
  color: var(--text);
}
.logout { margin-top: auto; }
main { padding: 1.25rem 1.5rem; overflow: auto; }
@media (max-width: 720px) {
  .shell { grid-template-columns: 1fr; }
  aside { border-right: 0; border-bottom: 1px solid var(--border); }
}
</style>
