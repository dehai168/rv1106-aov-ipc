<script setup lang="ts">
import { computed } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import {
  VideoCamera,
  InfoFilled,
  Setting,
  Monitor,
  Picture,
  Warning,
  FolderOpened,
  Connection,
  Tools,
  User,
  SwitchButton,
  Document,
} from '@element-plus/icons-vue'
import { ElMessageBox } from 'element-plus'
import { logout } from '../api'

const route = useRoute()
const router = useRouter()

const topTab = computed(() => {
  const p = route.path
  if (p.startsWith('/info')) return 'info'
  if (p.startsWith('/settings')) return 'settings'
  return 'preview'
})

const settingsActive = computed(() => route.path)

async function onLogout() {
  try {
    await ElMessageBox.confirm('确认退出登录？', '退出', {
      type: 'warning',
      confirmButtonText: '退出',
      cancelButtonText: '取消',
    })
  } catch {
    return
  }
  await logout()
  router.replace('/login')
}

function goTop(tab: string) {
  if (tab === 'preview') router.push('/preview')
  else if (tab === 'info') router.push('/info/device')
  else router.push('/settings/camera')
}
</script>

<template>
  <div class="layout">
    <header class="topbar">
      <div class="brand">
        <el-icon :size="20"><VideoCamera /></el-icon>
        <span class="brand-text">AOV IPC</span>
      </div>
      <nav class="top-tabs">
        <button
          type="button"
          class="top-tab"
          :class="{ active: topTab === 'preview' }"
          @click="goTop('preview')"
        >
          <el-icon><Monitor /></el-icon>
          实时预览
        </button>
        <button
          type="button"
          class="top-tab"
          :class="{ active: topTab === 'info' }"
          @click="goTop('info')"
        >
          <el-icon><InfoFilled /></el-icon>
          信息
        </button>
        <button
          type="button"
          class="top-tab"
          :class="{ active: topTab === 'settings' }"
          @click="goTop('settings')"
        >
          <el-icon><Setting /></el-icon>
          设置
        </button>
      </nav>
      <div class="top-right">
        <el-dropdown trigger="click">
          <span class="user-entry">
            <el-icon><User /></el-icon>
            admin
          </span>
          <template #dropdown>
            <el-dropdown-menu>
              <el-dropdown-item @click="router.push('/settings/system/password')">
                修改密码
              </el-dropdown-item>
              <el-dropdown-item divided @click="onLogout">
                <el-icon><SwitchButton /></el-icon>
                退出登录
              </el-dropdown-item>
            </el-dropdown-menu>
          </template>
        </el-dropdown>
      </div>
    </header>

    <div class="body" :class="{ 'with-side': topTab !== 'preview' }">
      <aside v-if="topTab === 'info'" class="side">
        <el-menu :default-active="settingsActive" router class="side-menu">
          <el-menu-item index="/info/device">
            <el-icon><Monitor /></el-icon>
            <span>设备信息</span>
          </el-menu-item>
          <el-menu-item index="/info/log">
            <el-icon><Document /></el-icon>
            <span>系统日志</span>
          </el-menu-item>
        </el-menu>
      </aside>

      <aside v-else-if="topTab === 'settings'" class="side">
        <el-menu :default-active="settingsActive" router class="side-menu">
          <el-sub-menu index="camera">
            <template #title>
              <el-icon><Picture /></el-icon>
              <span>摄像机</span>
            </template>
            <el-menu-item index="/settings/camera">图像与码流</el-menu-item>
          </el-sub-menu>
          <el-sub-menu index="event">
            <template #title>
              <el-icon><Warning /></el-icon>
              <span>事件</span>
            </template>
            <el-menu-item index="/settings/event/motion">移动侦测</el-menu-item>
          </el-sub-menu>
          <el-sub-menu index="storage">
            <template #title>
              <el-icon><FolderOpened /></el-icon>
              <span>存储</span>
            </template>
            <el-menu-item index="/settings/storage">存储与回放</el-menu-item>
          </el-sub-menu>
          <el-menu-item index="/settings/network">
            <el-icon><Connection /></el-icon>
            <span>网络</span>
          </el-menu-item>
          <el-sub-menu index="system">
            <template #title>
              <el-icon><Tools /></el-icon>
              <span>系统</span>
            </template>
            <el-menu-item index="/settings/system">基本设置</el-menu-item>
            <el-menu-item index="/settings/system/password">用户密码</el-menu-item>
          </el-sub-menu>
        </el-menu>
      </aside>

      <main class="main">
        <router-view />
      </main>
    </div>
  </div>
</template>

<style scoped>
.layout {
  height: 100%;
  display: grid;
  grid-template-rows: 52px 1fr;
  background: var(--ipc-bg);
}
.topbar {
  display: grid;
  grid-template-columns: 180px 1fr auto;
  align-items: center;
  gap: 1rem;
  padding: 0 1rem;
  background: var(--ipc-topbar);
  border-bottom: 1px solid var(--ipc-border);
  box-shadow: 0 1px 0 rgba(15, 30, 45, 0.04);
}
.brand {
  display: flex;
  align-items: center;
  gap: 0.45rem;
  color: var(--ipc-sidebar);
  font-weight: 700;
}
.brand-text { letter-spacing: 0.02em; }
.top-tabs { display: flex; gap: 0.35rem; justify-content: center; }
.top-tab {
  display: inline-flex;
  align-items: center;
  gap: 0.35rem;
  border: 0;
  background: transparent;
  color: var(--ipc-muted);
  padding: 0.45rem 0.9rem;
  border-radius: 6px;
  cursor: pointer;
  font: inherit;
}
.top-tab:hover { background: #f0f4f8; color: var(--ipc-text); }
.top-tab.active {
  background: rgba(43, 155, 187, 0.12);
  color: var(--ipc-accent);
  font-weight: 600;
}
.top-right { display: flex; justify-content: flex-end; }
.user-entry {
  display: inline-flex;
  align-items: center;
  gap: 0.35rem;
  cursor: pointer;
  color: var(--ipc-muted);
  font-size: 0.9rem;
}
.body {
  min-height: 0;
  display: grid;
  grid-template-columns: 1fr;
}
.body.with-side {
  grid-template-columns: 220px 1fr;
}
.side {
  background: var(--ipc-sidebar);
  overflow: auto;
}
.side-menu {
  border-right: 0;
  background: transparent;
  --el-menu-bg-color: transparent;
  --el-menu-text-color: var(--ipc-sidebar-text);
  --el-menu-hover-bg-color: rgba(255, 255, 255, 0.06);
  --el-menu-active-color: #fff;
}
.side :deep(.el-menu-item.is-active) {
  background: rgba(43, 155, 187, 0.28) !important;
}
.side :deep(.el-sub-menu__title:hover),
.side :deep(.el-menu-item:hover) {
  background: rgba(255, 255, 255, 0.06) !important;
}
.main {
  min-width: 0;
  overflow: auto;
  padding: 1.1rem 1.35rem 1.5rem;
}
@media (max-width: 860px) {
  .topbar { grid-template-columns: 1fr; padding: 0.5rem 0.75rem; gap: 0.5rem; }
  .body.with-side { grid-template-columns: 1fr; }
  .side { max-height: 220px; }
}
</style>
