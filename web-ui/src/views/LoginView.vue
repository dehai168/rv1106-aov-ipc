<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { login } from '../api'

const router = useRouter()
const username = ref('admin')
const password = ref('admin')
const error = ref('')
const loading = ref(false)

async function onSubmit() {
  error.value = ''
  loading.value = true
  try {
    const r = await login(username.value, password.value)
    if (r.code !== 0) {
      error.value = r.msg || '登录失败'
      return
    }
    await router.replace(r.data.must_change ? '/password' : '/preview')
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <div class="wrap">
    <form class="card" @submit.prevent="onSubmit">
      <h1>rv1106-aov-ipc</h1>
      <p class="muted">设备管理登录</p>
      <label>用户名<input v-model="username" autocomplete="username" /></label>
      <label>密码<input v-model="password" type="password" autocomplete="current-password" /></label>
      <p v-if="error" class="error">{{ error }}</p>
      <button :disabled="loading">{{ loading ? '…' : '登录' }}</button>
    </form>
  </div>
</template>

<style scoped>
.wrap {
  min-height: 100%;
  display: grid;
  place-items: center;
  padding: 1rem;
}
.card {
  width: min(380px, 100%);
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 12px;
  padding: 1.5rem;
  display: grid;
  gap: 0.75rem;
}
h1 { margin: 0; font-size: 1.4rem; }
label { display: grid; gap: 0.35rem; font-size: 0.9rem; color: var(--muted); }
input { width: 100%; }
button { margin-top: 0.5rem; }
</style>
