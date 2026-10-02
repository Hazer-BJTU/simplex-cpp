import { h } from 'vue';
import DefaultTheme from 'vitepress/theme';
import logo from '../../../assets/simplex-logo-v4.svg?url';
import './style.css';

export default {
    extends: DefaultTheme,
    Layout: () => h(DefaultTheme.Layout, null, {
        'home-hero-info-before': () => h('img', {
            src: logo,
            alt: 'Simplex — C++ Agent Harness',
            class: 'simplex-home-logo',
            width: 1400,
            height: 430,
        }),
    }),
};
