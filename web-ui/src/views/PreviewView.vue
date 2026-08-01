<script setup lang="ts">
import LivePreviewPlayer from '../components/LivePreviewPlayer.vue'
</script>

<template>
  <section class="preview-fill">
    <LivePreviewPlayer />
  </section>
</template>

<style scoped>
.preview-fill {
  height: 100%;
  width: 100%;
  min-height: 0;
}
</style>
