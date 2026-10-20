// Shapes and their areas.
export interface Circle {
  kind: "circle";
  radius: number;
}

export interface Rectangle {
  kind: "rectangle";
  width: number;
  height: number;
}

export type Shape = Circle | Rectangle;

export function area(shape: Shape): number {
  switch (shape.kind) {
    case "circle":
      return Math.PI * shape.radius ** 2;
    case "rectangle":
      return shape.width * shape.height;
  }
}

export function describe(shape: Shape, digits?: number): string {
  return `${shape.kind}: ${area(shape).toFixed(digits ?? 2)}`;
}
