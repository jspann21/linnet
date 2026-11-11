/** Linnet UI: host-only static TSX declarations. No JavaScript runtime. */
export interface NodeProps {
  x?: number; y?: number; w?: number; h?: number;
  fg?: string; bg?: string; flags?: number; count?: number; index?: number;
  resource?: string; text?: string; items?: readonly string[];
  targets?: readonly string[]; deps?: number; focus?: boolean;
  canvas?: string;
  children?: unknown;
}
export declare function Screen(props: NodeProps): unknown;
export declare function View(props: NodeProps): unknown;
export declare function Text(props: NodeProps): unknown;
export declare function Icon(props: NodeProps): unknown;
export declare function Image(props: NodeProps): unknown;
export declare function VirtualList(props: NodeProps): unknown;
export declare function Menu(props: NodeProps): unknown;
export declare function Grid(props: NodeProps): unknown;
export declare function Scrollbar(props: NodeProps): unknown;
export declare function ProgressBar(props: NodeProps): unknown;
export declare function StatusBar(props: NodeProps): unknown;
export declare function Dialog(props: NodeProps): unknown;
export declare function Toast(props: NodeProps): unknown;
export declare function AlbumArt(props: NodeProps): unknown;
export declare function PhotoSurface(props: NodeProps): unknown;
export declare function VideoSurface(props: NodeProps): unknown;
export declare function CanvasSurface(props: NodeProps): unknown;
