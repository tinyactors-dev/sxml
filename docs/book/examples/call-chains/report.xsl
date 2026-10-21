<?xml version="1.0" encoding="UTF-8"?>
<!-- The diff of two versions as an HTML page: the calls that changed,
     the call chains, and how often functions call other modules. -->
<xsl:stylesheet version="3.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">

  <xsl:output method="html" html-version="5" indent="yes"/>

  <xsl:template match="/diff">
    <html>
      <head>
        <title>Call chains, {@old} and {@new}</title>
        <style>
          .added {{ background: #dfd; }}
          .removed {{ background: #fdd; text-decoration: line-through; }}
          .changed {{ background: #ffd; }}
        </style>
      </head>
      <body>
        <h1>Call chains, {@old} and {@new}</h1>
        <h2>Calls that changed</h2>
        <table>
          <tr><th>Caller</th><th>Callee</th><th>Change</th></tr>
          <xsl:apply-templates select="call[@change]"/>
        </table>
        <h2>Chains</h2>
        <ul>
          <xsl:apply-templates select="chain"/>
        </ul>
        <h2>Calls to other modules</h2>
        <table>
          <tr><th>Function</th><th>Module</th><th>{@old}</th><th>{@new}</th></tr>
          <xsl:apply-templates select="module"/>
        </table>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="call">
    <tr class="{@change}">
      <td><code>{@from}</code></td>
      <td><code>{@to}</code>{if (@repeated) then ', in a loop' else ()}</td>
      <td>{@change}</td>
    </tr>
  </xsl:template>

  <xsl:template match="chain">
    <li><code>{.}</code></li>
  </xsl:template>

  <xsl:template match="chain[@change]">
    <li class="{@change}"><code>{.}</code> ({@change})</li>
  </xsl:template>

  <xsl:template match="module">
    <tr>
      <td><code>{@from}</code></td>
      <td><code>{@name}</code></td>
      <td>{@old}</td>
      <td>{@new}</td>
    </tr>
  </xsl:template>
</xsl:stylesheet>
