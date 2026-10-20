type Props = { name: string; avatar?: string; bio?: string };

export function Profile({ name, avatar, bio }: Props) {
  return (
    <div className="profile">
      <Avatar src={avatar} size={48} />
      <h2>{name}</h2>
      {bio && <Markdown text={bio} />}
    </div>
  );
}
