<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { changePassword } from '../api'
import PageShell from '../components/PageShell.vue'

const router = useRouter()
const oldp = ref('')
const newp = ref('')
const newp2 = ref('')
const loading = ref(false)

async function onSubmit() {
  if (!oldp.value) {
    ElMessage.warning('请输入旧密码')
    return
  }
  if (newp.value.length < 6) {
    ElMessage.warning('新密码至少 6 位')
    return
  }
  if (newp.value !== newp2.value) {
    ElMessage.warning('两次新密码不一致')
    return
  }
  loading.value = true
  try {
    const r = await changePassword(oldp.value, newp.value)
    if (r.code !== 0) {
      ElMessage.error(r.msg || '修改失败')
      return
    }
    ElMessage.success('密码已修改')
    await router.replace('/preview')
  } catch (e) {
    ElMessage.error(String(e))
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <PageShell title="修改密码" subtitle="首次登录须修改默认密码后才能使用预览等功能。">
    <el-form label-width="100px" style="max-width: 480px" @submit.prevent="onSubmit">
      <el-form-item label="旧密码">
        <el-input v-model="oldp" type="password" show-password placeholder="请输入旧密码" />
      </el-form-item>
      <el-form-item label="新密码">
        <el-input v-model="newp" type="password" show-password placeholder="至少 6 位" />
      </el-form-item>
      <el-form-item label="确认新密码">
        <el-input v-model="newp2" type="password" show-password />
      </el-form-item>
      <el-form-item>
        <el-button type="primary" :loading="loading" @click="onSubmit">保存</el-button>
      </el-form-item>
    </el-form>
  </PageShell>
</template>
