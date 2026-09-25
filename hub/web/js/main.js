/**
 * @file panel entry point.
 *
 * The only module that touches the DOM at import time: everything else is
 * declarations and exported functions so each file can be load-checked by
 * plain Node.
 */
import { init } from './app.js';

init();
