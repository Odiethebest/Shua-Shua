import type { Persona } from "../engine";

interface Props {
  personas: Persona[];
  activeId: number;
  onSelect: (id: number) => void;
}

// The persona switcher, styled like Xiaohongshu's top category tabs: a scrollable
// row of text tabs, the active one in the accent color.
export default function PersonaTabs({ personas, activeId, onSelect }: Props) {
  return (
    <nav className="persona-tabs" aria-label="persona">
      {personas.map((p) => (
        <button
          key={p.id}
          type="button"
          className={p.id === activeId ? "persona-tab is-active" : "persona-tab"}
          onClick={() => onSelect(p.id)}
        >
          {p.label}
        </button>
      ))}
    </nav>
  );
}
