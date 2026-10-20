<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>

  <xsl:template match="/">
    <html>
      <head><title>Library</title></head>
      <body>
        <xsl:apply-templates select="library/shelf"/>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="shelf">
    <h2>{@name}</h2>
    <ul>
      <xsl:apply-templates select="book"/>
    </ul>
  </xsl:template>

  <xsl:template match="book">
    <li><cite>{title}</cite> by {string-join(author, ' and ')}</li>
  </xsl:template>

  <xsl:template match="book[@year &lt; 1970]" priority="1">
    <li class="classic"><cite>{title}</cite> by {string-join(author, ' and ')}</li>
  </xsl:template>
</xsl:stylesheet>
