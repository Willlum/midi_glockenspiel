async function listFiles() {
  const res = await fetch('/api/files');
  const files = await res.json();
  const ul = document.getElementById('files');
  ul.innerHTML = '';
  files.forEach(name => {
    const li = document.createElement('li');
    li.textContent = name;
    const play = document.createElement('button'); play.textContent = 'Play';
    play.onclick = () => fetch('/api/play?filename=' + encodeURIComponent(name), {method: 'POST'});
    const del = document.createElement('button'); del.textContent = 'Delete';
    del.onclick = async () => { await fetch('/api/file?filename=' + encodeURIComponent(name), {method: 'DELETE'}); listFiles(); };
    const dl = document.createElement('a'); dl.textContent = 'Download'; dl.href = '/files/' + encodeURIComponent(name);
    li.appendChild(document.createTextNode(' '));
    li.appendChild(play);
    li.appendChild(document.createTextNode(' '));
    li.appendChild(del);
    li.appendChild(document.createTextNode(' '));
    li.appendChild(dl);
    ul.appendChild(li);
  });
}

document.getElementById('upload').onclick = async () => {
  const f = document.getElementById('fileinput').files[0];
  if (!f) return alert('Choose file');
  const path = f.name;
  const data = await f.arrayBuffer();
  await fetch('/api/upload?filename=' + encodeURIComponent(path), {method: 'POST', body: data});
  listFiles();
};

listFiles();
