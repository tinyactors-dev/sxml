import { Photo } from "./Photo.jsx";
import { Footer } from "./Footer.jsx";

export function Gallery({ photos }) {
  return (
    <section className="gallery">
      <h2>Photos</h2>
      {photos.map((photo) => (
        <Photo key={photo.id} src={photo.url} />
      ))}
      <Footer />
    </section>
  );
}
