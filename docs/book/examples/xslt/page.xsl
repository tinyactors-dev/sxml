<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>
  <xsl:param name="currency" select="'USD'"/>

  <xsl:template match="/library">
    <html>
      <body>
        <!-- The same books, processed twice in two modes. -->
        <ol class="contents">
          <xsl:apply-templates select="//book" mode="contents">
            <xsl:sort select="title"/>
          </xsl:apply-templates>
        </ol>
        <xsl:apply-templates select="//book[price/@currency = $currency]"/>
      </body>
    </html>
  </xsl:template>

  <xsl:template match="book" mode="contents">
    <li><a href="#{@id}">{title}</a></li>
  </xsl:template>

  <xsl:template match="book">
    <section id="{@id}">
      <h2>{title}</h2>
      <xsl:call-template name="price">
        <xsl:with-param name="amount" select="xs:decimal(price)"/>
      </xsl:call-template>
    </section>
  </xsl:template>

  <xsl:template name="price">
    <xsl:param name="amount" as="xs:decimal"/>
    <p>
      <xsl:choose>
        <xsl:when test="$amount lt 20">A bargain: </xsl:when>
        <xsl:when test="$amount gt 100">An investment: </xsl:when>
      </xsl:choose>
      <xsl:text>{format-number($amount, '0.00')} {$currency}</xsl:text>
    </p>
  </xsl:template>
</xsl:stylesheet>
