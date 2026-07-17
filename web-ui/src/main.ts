import { createApp } from 'vue'
import App from './App.vue'
import router from './router'
import 'element-plus/es/components/message/style/css'
import 'element-plus/es/components/message-box/style/css'
import './styles.css'

createApp(App).use(router).mount('#app')
