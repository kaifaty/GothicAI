// The en RSS feed (written by modules/site.ts), served at its address and prerendered to a file.
import { defineEventHandler, setHeader } from "h3";
import feeds from "#site-feeds";

export default defineEventHandler((event) => {
  setHeader(event, "content-type", "application/rss+xml; charset=utf-8");
  return feeds.en;
});
