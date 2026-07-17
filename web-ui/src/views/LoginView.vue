<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { VideoCamera } from '@element-plus/icons-vue'
import { login } from '../api'

const router = useRouter()
const username = ref('admin')
const password = ref('admin')
const loading = ref(false)

async function onSubmit() {
  loading.value = true
  try {
    const r = await login(username.value, password.value)
    if (r.code !== 0) {
      ElMessage.error(r.msg || '登录失败')
      return
    }
    ElMessage.success('登录成功')
    await router.replace(r.data.must_change ? '/settings/system/password' : '/preview')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="login-page">
    <div class="hero" />
    <el-card class="login-card" shadow="always">
      <div class="logo">
        <el-icon :size="28" color="var(--ipc-accent)"><VideoCamera /></el-icon>
        <div>
          <h1>AOV IPC</h1>
          <p>设备管理登录</p>
        </div>
      </div>
      <el-form label-position="top" @submit.prevent="onSubmit">
        <el-form-item label="用户名">
          <el-input v-model="username" autocomplete="username" clearable />
        </el-form-item>
        <el-form-item label="密码">
          <el-input
            v-model="password"
            type="password"
            show-password
            autocomplete="current-password"
            @keyup.enter="onSubmit"
          />
        </el-form-item>
        <el-button type="primary" class="submit" :loading="loading" native-type="submit" @click="onSubmit">
          登录
        </el-button>
      </el-form>
    </el-card>
  </div>
</template>

<style scoped>
.login-page {
  min-height: 100%;
  display: grid;
  place-items: center;
  position: relative;
  overflow: hidden;
  padding: 1.5rem;
}
.hero {
  position: absolute;
  inset: 0;
  background:
    radial-gradient(900px 420px at 15% 20%, rgba(43, 155, 187, 0.28), transparent 60%),
    radial-gradient(700px 380px at 85% 80%, rgba(31, 42, 55, 0.18), transparent 55%),
    linear-gradient(160deg, #dfe8ef 0%, #eef1f5 45%, #d7e3ea 100%);
}
.login-card {
  position: relative;
  width: min(400px, 100%);
  border-radius: 12px;
  border: 1px solid rgba(255, 255, 255, 0.7);
}
.logo {
  display: flex;
  gap: 0.75rem;
  align-items: center;
  margin-bottom: 1.25rem;
}
.logo h1 {
  margin: 0;
  font-size: 1.35rem;
}
.logo p {
  margin: 0.15rem 0 0;
  color: var(--ipc-muted);
  font-size: 0.85rem;
}
.submit { width: 100%; margin-top: 0.25rem; }
</style>
