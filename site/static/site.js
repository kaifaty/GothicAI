// The short on a page: a muted loop plays unless the reader asked for less motion (with a button to stop
// it), a YouTube video loads only when it is asked for. Things marked data-reveal rise into place once as
// they come into view; the claim on the home page lights up word by word as it is scrolled through.
(function () {
  window.gothicSite = true;  // the page's inline script shows everything if this never runs
  var ru = document.documentElement.lang === "ru";
  var still = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  document.querySelectorAll("video.loop").forEach(function (video) {
    var button = document.createElement("button");
    button.type = "button";
    button.className = "toggle";
    function label() {
      button.textContent = video.paused ? (ru ? "Пустить" : "Play") : (ru ? "Пауза" : "Pause");
    }
    button.addEventListener("click", function () {
      if (video.paused) video.play(); else video.pause();
    });
    video.addEventListener("play", label);
    video.addEventListener("pause", label);
    video.parentNode.appendChild(button);
    label();
    if (!still) {
      var p = video.play();
      if (p && p.catch) p.catch(label);
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
