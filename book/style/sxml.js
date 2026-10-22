// A link from the book's menu bar back to the website's landing page.
(function () {
    var buttons = document.querySelector('#menu-bar .right-buttons');
    if (!buttons) {
        return;
    }
    var home = document.createElement('a');
    home.className = 'sxml-home';
    home.href = '/';
    home.title = 'sxml.tinyactors.dev';
    home.textContent = 'sxml';
    buttons.insertBefore(home, buttons.firstChild);
})();
