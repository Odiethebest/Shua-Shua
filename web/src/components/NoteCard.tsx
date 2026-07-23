import type { FeedItem } from "../engine";
import { contentFor } from "../presentation";

function HeartIcon() {
  return (
    <svg className="heart" viewBox="0 0 24 24" width="13" height="13" aria-hidden="true">
      <path
        d="M12 20.3l-1.32-1.2C6 14.98 2.75 12.03 2.75 8.42 2.75 5.9 4.67 4 7.15 4c1.5 0 2.96.78 3.85 2.02h1c.89-1.24 2.35-2.02 3.85-2.02 2.48 0 4.4 1.9 4.4 4.42 0 3.61-3.25 6.56-7.93 10.68L12 20.3z"
        fill="none"
        stroke="currentColor"
        strokeWidth="1.7"
      />
    </svg>
  );
}

// One Xiaohongshu-style note card: cover on top, then a 2-line title, the "why"
// caption, and an author + likes row. `variant` is the item's position among
// same-category cards, used to keep titles from repeating.
export default function NoteCard({ item, variant }: { item: FeedItem; variant: number }) {
  const c = contentFor(item, variant);
  return (
    <article className="card">
      <div
        className="cover"
        style={{
          background: c.cover.gradient,
          paddingBottom: `${c.cover.aspect * 100}%`,
        }}
      >
        <span className="cover-emoji">{c.cover.emoji}</span>
      </div>
      <div className="card-body">
        <h3 className="card-title">{c.title}</h3>
        <p className="card-why">{c.why}</p>
        <div className="card-meta">
          <div className="author">
            <span className="avatar" style={{ background: c.avatar.gradient }}>
              {c.avatar.letter}
            </span>
            <span className="author-name">{c.author}</span>
          </div>
          <div className="likes">
            <HeartIcon />
            <span>{c.likes}</span>
          </div>
        </div>
      </div>
    </article>
  );
}
