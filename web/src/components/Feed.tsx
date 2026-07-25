import { useEffect, useState } from "react";
import Masonry from "react-masonry-css";
import type { FeedItem } from "../engine";
import { coversLoaded, loadCovers } from "../covers";
import NoteCard from "./NoteCard";

// Responsive column counts (keys are max window widths), mirroring the reference:
// wide screens show up to 5 columns, narrow screens 2.
const breakpointCols = { default: 5, 1280: 4, 980: 3, 680: 2 };

// Load the local cover manifest once, re-rendering when it's ready so cards can
// swap the gradient placeholder for a real cover.
function useCovers(): void {
  const [, bump] = useState(0);
  useEffect(() => {
    if (coversLoaded()) return;
    let cancelled = false;
    loadCovers().then(() => {
      if (!cancelled) bump((n) => n + 1);
    });
    return () => {
      cancelled = true;
    };
  }, []);
}

interface Props {
  items: FeedItem[];
  onCardClick: (id: number, category: string) => void;
}

export default function Feed({ items, onCardClick }: Props) {
  useCovers();

  return (
    <Masonry
      breakpointCols={breakpointCols}
      className="feed-grid"
      columnClassName="feed-grid_column"
    >
      {items.map((item) => (
        <NoteCard key={item.id} item={item} onCardClick={onCardClick} />
      ))}
    </Masonry>
  );
}
