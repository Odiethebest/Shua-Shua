import Masonry from "react-masonry-css";
import type { FeedItem } from "../engine";
import NoteCard from "./NoteCard";

// Two-column masonry of varying-height cards — the Xiaohongshu feed layout.
export default function Feed({ items }: { items: FeedItem[] }) {
  // Count items per category as we go, so each card gets its position within its
  // category — presentation.contentFor uses it to avoid repeating titles.
  const seenPerCategory: Record<string, number> = {};
  return (
    <Masonry
      breakpointCols={2}
      className="feed-grid"
      columnClassName="feed-grid_column"
    >
      {items.map((item) => {
        const variant = seenPerCategory[item.category] ?? 0;
        seenPerCategory[item.category] = variant + 1;
        return <NoteCard key={item.id} item={item} variant={variant} />;
      })}
    </Masonry>
  );
}
