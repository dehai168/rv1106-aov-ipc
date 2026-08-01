<script setup lang="ts">
withDefaults(
  defineProps<{
    title?: string
    subtitle?: string
    flush?: boolean
  }>(),
  {
    title: '',
    subtitle: '',
    flush: false,
  },
)
</script>

<template>
  <section class="page-shell" :class="{ flush }">
    <header v-if="title || subtitle" class="page-head">
      <h2 v-if="title" class="page-title">{{ title }}</h2>
      <p v-if="subtitle" class="page-desc">{{ subtitle }}</p>
    </header>
    <div class="page-body">
      <slot />
    </div>
  </section>
</template>

<style scoped>
.page-shell {
  min-height: 100%;
  display: flex;
  flex-direction: column;
  gap: 0.85rem;
}
.page-shell.flush {
  gap: 0;
  height: 100%;
}
.page-head {
  flex: 0 0 auto;
}
.page-body {
  flex: 1 1 auto;
  min-height: 0;
  background: #fff;
  border: 1px solid var(--ipc-border);
  border-radius: 8px;
  padding: 1rem 1.1rem;
  overflow: auto;
}
.page-shell.flush .page-body {
  border: 0;
  border-radius: 0;
  padding: 0;
  background: #0b0f14;
}
</style>
