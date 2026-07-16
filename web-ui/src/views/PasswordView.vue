<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { changePassword } from '../api'

const router = useRouter()
const oldp = ref('admin')
const newp = ref('')
const newp2 = ref('')
const error = ref('')
const ok = ref('')
const loading = ref(false)

async function onSubmit() {
  error.value = ''
  ok.value = ''
  if (newp.value.length < 6) {
    error.value = '新密码至少 6 位'
    return
  }
  if (newp.value !== newp2.value) {
    error.value = '两次新密码不一致'
    return
  }
  loading.value = true
  try {
    const r = await changePassword(oldp.value, newp.value)
    if (r.code !== 0) {
      error.value = r.msg || '修改失败'
      return
    }
    ok.value = '已修改，正在进入预览…'
    await router.replace('/preview')
  } catch (e) {
    error.value = String(e)
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <section class="pw">
    <h2>修改密码</h2>
    <p class="muted">首次登录须修改默认密码后才能使用预览等功能。</p>
    <form @submit.prevent="onSubmit">
      <label>旧密码<input v-model="oldp" type="password" /></label>
      <label>新密码<input v-model="newp" type="password" /></label>
      <label>确认新密码<input v-model="newp2" type="password" /></label>
      <p v-if="error" class="error">{{ error }}</p>
      <p v-if="ok" class="muted">{{ ok }}</p>
      <button :disabled="loading">{{ loading ? '…' : '保存' }}</button>
    </form>
  </section>
</template>

<style scoped>
.pw { max-width: 420px; display: grid; gap: 0.75rem; }
form { display: grid; gap: 0.75rem; }
label { display: grid; gap: 0.35rem; color: var(--muted); font-size: 0.9rem; }
</style>
