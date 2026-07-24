import { useState } from "react";
import type { FeedItem } from "../engine";
import { contentFor } from "../presentation";
import { coverFor } from "../covers";

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
//
// The cover is a local (committed) Unsplash photo when one exists for this
// category; otherwise it falls back to the gradient + emoji placeholder. The emoji
// sits behind the image, so it also shows while the image is still loading.
interface Props {
  item: FeedItem;
  variant: number;
  onOpenItem: (id: number, title: string) => void;
}

export default function NoteCard({ item, variant, onOpenItem }: Props) {
  const c = contentFor(item, variant);
  const cover = coverFor(item.category, item.id);
  const [imgLoaded, setImgLoaded] = useState(false);
  const [imgFailed, setImgFailed] = useState(false);

  // Clicking a card re-runs the engine with THIS item's vector as the query
  // ("more like this"). The attribution links stopPropagation so they don't
  // trigger this.
  return (
    <article className="card" onClick={() => onOpenItem(item.id, c.title)}>
      <div
        className="cover"
        style={{
          background: c.cover.gradient,
          paddingBottom: `${c.cover.aspect * 100}%`,
        }}
      >
        <span className="cover-emoji">{c.cover.emoji}</span>
        {cover !== null && !imgFailed && (
          <>
            <img
              className={imgLoaded ? "cover-img is-loaded" : "cover-img"}
              src={`/${cover.file}`}
              alt=""
              loading="lazy"
              onLoad={() => setImgLoaded(true)}
              onError={() => setImgFailed(true)}
            />
            {imgLoaded && (
              <div className="cover-credit" onClick={(e) => e.stopPropagation()}>
                Photo by{" "}
                <a href={cover.photographerUrl} target="_blank" rel="noopener noreferrer">
                  {cover.photographer}
                </a>{" "}
                on{" "}
                <a href={cover.unsplashUrl} target="_blank" rel="noopener noreferrer">
                  Unsplash
                </a>
              </div>
            )}
          </>
        )}
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
