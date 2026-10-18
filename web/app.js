document.addEventListener('DOMContentLoaded', () => {
    const explodeBtn = document.getElementById('toggleExplodeBtn');
    const scene = document.getElementById('deviceScene');

    // Toggle Exploded View
    explodeBtn.addEventListener('click', () => {
        scene.classList.toggle('exploded');
        if (scene.classList.contains('exploded')) {
            explodeBtn.textContent = 'Собрать устройство';
        } else {
            explodeBtn.textContent = 'Убрать корпус и раздвинуть детали';
        }
    });

    // Simulation State
    let isPlaying = false;
    let progress = 0;
    let progressInterval = null;

    const playIcon = document.getElementById('sim-play-icon');
    const progressBar = document.getElementById('sim-progress');
    const titleEl = document.getElementById('sim-title');
    const artistEl = document.getElementById('sim-artist');
    const albumEl = document.getElementById('sim-album');

    const tracks = [
        { title: "Bohemian Rhapsody", artist: "Queen", icon: "👑" },
        { title: "Blinding Lights", artist: "The Weeknd", icon: "🔴" },
        { title: "Hotel California", artist: "Eagles", icon: "🏨" },
        { title: "Smells Like Teen Spirit", artist: "Nirvana", icon: "🎸" }
    ];
    let currentTrackIdx = 0;

    function updateScreen() {
        playIcon.textContent = isPlaying ? '▶' : '⏸';
        titleEl.textContent = tracks[currentTrackIdx].title;
        artistEl.textContent = tracks[currentTrackIdx].artist;
        albumEl.textContent = tracks[currentTrackIdx].icon;
        progressBar.style.width = `${progress}%`;
    }

    function togglePlay() {
        isPlaying = !isPlaying;
        if (isPlaying) {
            progressInterval = setInterval(() => {
                progress += 1;
                if (progress > 100) {
                    progress = 0;
                    nextTrack();
                } else {
                    progressBar.style.width = `${progress}%`;
                }
            }, 500);
        } else {
            clearInterval(progressInterval);
        }
        updateScreen();
    }

    function nextTrack() {
        currentTrackIdx = (currentTrackIdx + 1) % tracks.length;
        progress = 0;
        updateScreen();
    }

    function prevTrack() {
        currentTrackIdx = (currentTrackIdx - 1 + tracks.length) % tracks.length;
        progress = 0;
        updateScreen();
    }

    // Button Listeners
    document.querySelector('.button.play').addEventListener('click', togglePlay);
    document.querySelector('.button.next').addEventListener('click', nextTrack);
    document.querySelector('.button.prev').addEventListener('click', prevTrack);
    document.querySelector('.center-btn').addEventListener('click', () => {
        // Simulate Like
        titleEl.style.color = titleEl.style.color === 'red' ? '#111' : 'red';
    });

    // Initial render
    updateScreen();
});