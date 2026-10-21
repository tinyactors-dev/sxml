<!-- The trace diff (from diff-xml.xq) as an HTML report. With the
     parameter drawings, a list of SVG files from drawing.xq, each route
     shows the drawing whose title names it. -->
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:svg="http://www.w3.org/2000/svg"
    exclude-result-prefixes="svg">
  <xsl:output method="html" html-version="5" indent="yes"/>
  <xsl:param name="drawings" select="''"/>

  <xsl:template match="/diff">
    <html>
      <head>
        <title>Trace diff: {@before} and {@after}</title>
        <style>
          td, th {{ padding: 2px 12px; text-align: left }}
          .added {{ color: #2e7d32 }}
          .removed {{ color: #c62828; text-decoration: line-through }}
          .changed {{ color: #b26a00 }}
        </style>
      </head>
      <body>
        <h1>Trace diff</h1>
        <p>From <code>{@before}</code> to <code>{@after}</code>.</p>
        <h2>Versions</h2>
        <table>
          <tr><th>Service</th><th>Before</th><th>After</th></tr>
          <xsl:apply-templates select="version"/>
        </table>
        <xsl:for-each-group select="change" group-by="@route">
          <h2>{current-grouping-key()}</h2>
          <table>
            <tr><th>Caller</th><th>Callee</th><th>Before</th><th>After</th></tr>
            <xsl:apply-templates select="current-group()"/>
          </table>
          <xsl:copy-of select="tokenize($drawings) ! doc(.)
              /svg:svg[starts-with(svg:title, current-grouping-key() || ':')]"/>
        </xsl:for-each-group>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="version">
    <tr><td>{@service}</td><td>{(@before, 'none')[1]}</td><td>{(@after, 'none')[1]}</td></tr>
  </xsl:template>

  <!-- A call that is new, gone, or made a different number of times. -->
  <xsl:template match="change">
    <xsl:param name="kind" select="'changed'"/>
    <tr class="{$kind}">
      <td>{@caller}</td><td>{@callee}</td><td>{@before}</td><td>{@after}</td>
    </tr>
  </xsl:template>

  <xsl:template match="change[@before = 0]" priority="1">
    <xsl:next-match><xsl:with-param name="kind" select="'added'"/></xsl:next-match>
  </xsl:template>

  <xsl:template match="change[@after = 0]" priority="1">
    <xsl:next-match><xsl:with-param name="kind" select="'removed'"/></xsl:next-match>
  </xsl:template>
</xsl:stylesheet>
