// The short on a page: a muted loop loads nothing until it comes near the screen, then plays unless the reader
// asked for less motion (with a button to stop it); a YouTube video loads only when it is asked for. Things
// marked data-reveal rise into place once as they come into view; the claim on the home page lights up word
// by word as it is scrolled through; the backgrounds below the first screen load as it nears them. Minified and inlined into every page when the site is built.
(function () {
  window.gothicSite = true;  // the page's inline script shows everything if this never runs
  var ru = document.documentElement.lang === "ru";
  var still = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  document.querySelectorAll("video.loop").forEach(function (video) {
    var source = video.querySelector("source[data-src]");
    var button = document.createElement("button");
    button.type = "button";
    button.className = "toggle";
    function label() {
      button.textContent = video.paused ? (ru ? "Пустить" : "Play") : (ru ? "Пауза" : "Pause");
    }
    // the clip's address goes in only now: before it the page asks for nothing but the poster
    function load() {
      if (source && !source.getAttribute("src")) {
        source.setAttribute("src", source.getAttribute("data-src"));
        video.load();
      }
    }
    function play() {
      load();
      var p = video.play();
      if (p && p.catch) p.catch(label);
    }
    button.addEventListener("click", function () {
      if (video.paused) play(); else video.pause();
    });
    video.addEventListener("play", label);
    video.addEventListener("pause", label);
    video.parentNode.appendChild(button);
    label();
    if (still) return;
    if ("IntersectionObserver" in window) {
      // near: less than the section's own top padding below the screen, so the first screen never loads it
      var near = new IntersectionObserver(function (entries) {
        if (entries[0].isIntersecting) {
          near.disconnect();
          play();
        }
      }, { rootMargin: "0px 0px 64px 0px" });
      near.observe(video);
    } else {
      play();
    }
  });

  document.querySelectorAll("button.yt").forEach(function (button) {
    button.addEventListener("click", function () {
      var frame = document.createElement("iframe");
      frame.src = "https://www.youtube-nocookie.com/embed/" + encodeURIComponent(button.dataset.id) +
        "?autoplay=1&playsinline=1&rel=0";
      frame.title = button.getAttribute("aria-label") || "YouTube";
      frame.allow = "autoplay; encrypted-media; picture-in-picture; fullscreen";
      frame.allowFullscreen = true;
      button.replaceWith(frame);
      frame.focus();
    });
  });

  // Neighbours in one row arrive one after another; the steps of the track light up in order.
  document.querySelectorAll(".cards, .features").forEach(function (list) {
    Array.prototype.forEach.call(list.children, function (item, i) {
      item.style.setProperty("--i", i % 4);
    });
  });
  document.querySelectorAll(".flow li").forEach(function (item, i) {
    item.style.setProperty("--i", i);
  });

  var marked = document.querySelectorAll("[data-reveal]");
  if ("IntersectionObserver" in window) {
    var seen = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-in");
          seen.unobserve(entry.target);
        }
      });
    }, { rootMargin: "0px 0px -10% 0px", threshold: 0.15 });
    marked.forEach(function (el) { seen.observe(el); });
  } else {
    marked.forEach(function (el) { el.classList.add("is-in"); });
  }

  // The diary's paper and the footer's valley (backgrounds, which a browser can not load lazily) are asked
  // for one screen before they come into view.
  var painted = document.querySelectorAll(".diary, .foot");
  if ("IntersectionObserver" in window) {
    var soon = new IntersectionObserver(function (entries) {
      entries.forEach(function (entry) {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-near");
          soon.unobserve(entry.target);
        }
      });
    }, { rootMargin: "100% 0px" });
    painted.forEach(function (el) { soon.observe(el); });
  } else {
    painted.forEach(function (el) { el.classList.add("is-near"); });
  }

  var claim = document.querySelector("[data-words]");
  if (claim && !still) {
    var words = claim.textContent.trim().split(/\s+/);
    claim.textContent = "";
    words.forEach(function (word, i) {
      var span = document.createElement("span");
      span.className = "w";
      span.textContent = word;
      claim.appendChild(span);
      if (i < words.length - 1) claim.appendChild(document.createTextNode(" "));
    });
    var spans = claim.querySelectorAll(".w");
    var queued = false;
    // The words light up in order while the sentence rises from near the bottom of the screen to a third
    // of the way down it: half a screen of scrolling for the whole sentence.
    var update = function () {
      queued = false;
      var top = claim.getBoundingClientRect().top;
      var screen = window.innerHeight;
      var share = (screen * 0.85 - top) / (screen * 0.5);
      var lit = Math.round(Math.min(Math.max(share, 0), 1) * spans.length);
      spans.forEach(function (span, i) { span.classList.toggle("on", i < lit); });
    };
    window.addEventListener("scroll", function () {
      if (!queued) { queued = true; window.requestAnimationFrame(update); }
    }, { passive: true });
    window.addEventListener("resize", update);
    update();
  }
})();
